# RPi Kernel Driver — Debug Log

## Day 1: 環境建置 + Hello World Module
- 兩台 RPi 環境建置（含 kernel headers 版本對齊排查）
- 踩坑：kernel 版本與 apt headers 沒對齊，需 apt full-upgrade 才抓到正確 headers
- 第一個 kernel module 在兩台上都驗證成功
- 完整生命週期：insmod → dmesg 確認 → rmmod → dmesg 確認
- 核心概念：module_init/exit 生命週期、printk vs printf、__init/__exit、insmod/rmmod

## Day 2: Character Driver
- file_operations 把 open/read/write/close 對應到自己的函式
- copy_from_user (write): user→kernel，因為位址空間隔離不能直接 memcpy
- copy_to_user (read): kernel→user
- EOF 處理：*offset >= buffer_len 時 return 0，避免 cat 無限迴圈
  (踩過的理解：offset 從 0→6，第二次 read 觸發 EOF)
- major number 自動分配 (register_chrdev(0,...))，每次載入可能不同
- 測試：echo 寫入 + cat 讀出，dmesg 看到完整 open→write→read→close 生命週期

## Day 3: ioctl + poll

### ioctl
- ioctl 處理「控制命令」（read/write 只能傳資料流，ioctl 下命令：CLEAR/GET_LEN）
- _IO / _IOR macro 生成唯一命令號碼，user 和 kernel 必須共用相同定義才對得上
- GET_LEN 用 copy_to_user 把 buffer_len 傳回 user
- 測試：寫 user 程式 (gcc 編譯) 呼叫 ioctl，不能用 echo/cat

### poll
- poll 讓 process 睡著等事件，不忙輪詢（跟 FreeRTOS queue 阻塞同理）
- 機制：wait queue + poll_wait（登記睡覺）+ wake_up_interruptible（喚醒）
- 核心：write 改變狀態(有資料了)，所以 write 要 wake_up 叫醒在等的 process
  （像快遞放包裹要按門鈴，不然等的人永遠不知道）
- 踩坑：編輯時 my_write 留了重複的 printk/return/} 殘留，導致編譯錯誤
  (錯誤行號往後指，真正問題在函式外的孤立 code)
- 測試：兩終端，終端1 poll 等待，終端2 write 觸發喚醒

## Day 4: Network Driver

- network driver vs character driver：net_device(網路介面) vs /dev檔案；封包 vs bytes
- 結構：alloc_etherdev → net_device_ops → register_netdev（跟 char 的 register_chrdev 相通）
- ndo_start_xmit 是核心：送封包時 kernel 呼叫，處理 sk_buff
- sk_buff：裝封包的結構，skb->len=長度，用完 dev_kfree_skb 釋放
  （記憶體所有權，跟 malloc/free 同理，不釋放=kernel memory leak）
- 測試：insmod → ip link show mynet0 → ip link set up → ping 觸發 xmit

### Day 4 續：實現真正 peer-to-peer 雙向通訊

- 問題1：加 NOARP 前，ping 出現 Destination Host Unreachable，
  my_xmit 完全沒被觸發
  → root cause：alloc_etherdev 建立的乙太網路介面，kernel 送封包前
  會先嘗試 ARP 解析，driver 沒實作 ARP 回應導致解析失敗
  → fix：my_dev->flags |= IFF_NOARP，跳過 ARP 機制

- 問題2：加 NOARP 後，dmesg 確認 xmit 真的被觸發（98 bytes，
  對應 ICMP ping），UDP 也真的送出去了，但 ping 仍 100% loss
  → root cause：my_xmit 只有「送」的邏輯，對方沒有程式監聽
  port 12345，UDP 送達後直接被系統丟棄

- 實作接收端：
  - 為什麼不能放 module_init 裡：insmod 是同步 syscall，
    init 必須有限時間內返回；接收資料本質上是阻塞等待，
    必須用獨立 kernel thread（kthread_run）
  - recv_sock 用 kernel_bind() 綁定本機 port 12345
  - recv_thread_fn() 用 kernel_recvmsg() 阻塞等待資料
  - 收到後用 dev_alloc_skb + skb_reserve(2) + skb_put + memcpy
    組出 sk_buff，eth_type_trans 解析 header，
    netif_rx() 注入回 mynet0

- 問題3：重新 insmod 後 ping 又失敗，來源出現詭異的 10.0.0.242
  → root cause：rmmod 會清掉介面 IP，重新載入是全新介面實例，
  忘記用 ip addr add 重新設定 IP 導致路由表沒有對應規則

- 最終結果：rpi1 ping rpi2 (10.0.0.2)，3/3 收到回應，0% packet loss
  雙方 dmesg 完整對應：transmitting → received → transmitting → received

- 關鍵發現：ICMP echo reply 是 kernel 自己的協定堆疊處理的，
  driver 完全沒寫任何 ICMP 邏輯——證明 netif_rx() 真的讓 kernel
  把這個封包當成「從網路收到的真實封包」

### Day 4 續：I2C Sensor Driver (BME280)

- 目標：透過 Linux kernel I2C 框架讀取 BME280 chip ID，
  對比 STM32 那次自己手寫 register-level I2C driver

- 核心概念：i2c_client vs i2c_driver 分離設計
  - i2c_client：代表「一個實際存在的裝置」（名稱+位址）
  - i2c_driver：代表「知道怎麼操作某類裝置的邏輯」
    (probe/remove + id_table)
  - 分離目的：同一份 driver 邏輯可套用在多個實際裝置上，
    不用為每個裝置重寫一份幾乎一樣的程式碼

- 因為沒有 device tree 描述這個手動接線的裝置，用
  i2c_get_adapter(1) + i2c_board_info + i2c_new_client_device()
  手動建立 client，模擬「kernel 偵測到裝置」

- probe() 觸發時機：kernel 在 client 和 driver 兩者都存在、
  名稱配對成功時自動呼叫 probe()。這次是先建立 client、後用
  i2c_add_driver() 註冊 driver，所以 probe() 實際上在
  i2c_add_driver() 那行被觸發（dmesg 訊息順序驗證：chip id
  訊息出現在 module loaded 之前）

- i2c_smbus_read_byte_data() 對比 STM32 手寫的 i2c1_read_reg()：
  同樣完成 START/address+write/register/repeated START/
  address+read/STOP 整套 I2C transaction，Linux 這邊由 kernel
  的 I2C controller driver 封裝掉底層細節，一行呼叫完成

- 踩坑：BME280 一開始誤接到 rpi1，i2cdetect -y 1 全部空白；
  切到 rpi2 才掃描到 0x76

- 踩坑：sudo shutdown -h now 誤用（該用 sudo reboot），
  導致機器完全關機，需手動拔插電源才能重開機

- 最終驗證：
  insmod → dmesg "chip id = 0x60" → "module loaded"
  rmmod  → dmesg "removed" → "module unloaded"
  chip id 0x60 與 STM32 讀到的完全一致，證明同一顆晶片，
  兩種不同抽象層次的實作方式都正確運作
