// rtp.c - 實現RTP音頻發送和接收功能
#include "sip_client.h"
#include <math.h>  // Add this to fix sinf() function reference
#include <sched.h>  // Add this for pthread_setschedparam

// 全局變量用於RTP接收
static pthread_t rtp_thread;
static int rtp_sockfd = -1;
static int running = 0;
static FILE *output_file = NULL;
static FILE *raw_data_file = NULL;  // 用於保存原始RTP數據
static int received_packet_count = 0;  // 添加計數器來跟踪收到的封包數量
static unsigned int total_bytes_received = 0;  // 添加計數器來跟踪收到的數據總量
static int real_audio_data_received = 0;  // 標記是否接收到實際RTP音頻數據

// 添加回調函數指針
typedef void (*rtp_data_callback_t)(const unsigned char *rtp_data, size_t data_size);
static rtp_data_callback_t global_rtp_callback = NULL;

// 設置RTP數據回調函數
void set_rtp_callback(rtp_data_callback_t callback) {
    global_rtp_callback = callback;
    log_with_timestamp("RTP接收器回調函數已設置\n");
}

// 清除RTP數據回調函數
void clear_rtp_callback(void) {
    global_rtp_callback = NULL;
    log_with_timestamp("RTP接收器回調函數已清除\n");
}

// 初始化RTP包頭
void init_rtp_header(rtp_header_t *hdr, int payload_type, unsigned short seq_num, 
                    unsigned int timestamp, unsigned int ssrc) {
    hdr->version_p_x_cc = 0x80;  // RTP版本2, 無填充, 無擴展, 無CSRC
    hdr->m_pt = payload_type & 0x7F;  // 無標記位, 負載類型 (0 for PCMU)
    hdr->seq_num = htons(seq_num);
    hdr->timestamp = htonl(timestamp);
    hdr->ssrc = htonl(ssrc);
}

// 發送RTP音頻數據包
void send_rtp_audio(int sockfd, struct sockaddr_in *dest_addr, const char *wav_file, int dest_port,
                   const char *callid, const char *tag, const char *to_tag, const char *cseq,
                   struct sockaddr_in *servaddr) {
    int fd;
    char buffer[RTP_PACKET_SIZE + sizeof(rtp_header_t)];
    rtp_header_t *rtp_hdr = (rtp_header_t *)buffer;
    char *payload = buffer + sizeof(rtp_header_t);
    int bytes_read;
    unsigned short seq_num = 0;
    unsigned int timestamp = 0;
    unsigned int ssrc = rand();  // 隨機SSRC
    int samples_per_packet = RTP_PACKET_SIZE;  // 每個RTP包中的樣本數
    struct stat st;
    
    log_with_timestamp("開始發送RTP音頻: %s -> %s:%d\n", 
                     wav_file, inet_ntoa(dest_addr->sin_addr), dest_port);
    
    // 設置目標地址的端口
    dest_addr->sin_port = htons(dest_port);
    
    // 打開WAV文件
    fd = open(wav_file, O_RDONLY);
    if (fd < 0) {
        log_with_timestamp("錯誤: 無法打開WAV文件 %s: %s\n", wav_file, strerror(errno));
        return;
    }
    
    // 獲取文件大小
    if (fstat(fd, &st) < 0) {
        log_with_timestamp("錯誤: 無法獲取文件狀態: %s\n", strerror(errno));
        close(fd);
        return;
    }
    
    log_with_timestamp("WAV文件大小: %ld 字節\n", (long)st.st_size);
    
    // 跳過WAV文件頭
    if (lseek(fd, WAV_HEADER_SIZE, SEEK_SET) < 0) {
        log_with_timestamp("錯誤: 無法跳過WAV頭: %s\n", strerror(errno));
        close(fd);
        return;
    }
    
    // 創建RTP socket
    int rtp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_sockfd < 0) {
        log_with_timestamp("錯誤: 無法創建RTP socket: %s\n", strerror(errno));
        close(fd);
        return;
    }
    
    // 綁定本地RTP端口 - 使用專用發送端口
    struct sockaddr_in local_rtp_addr;
    memset(&local_rtp_addr, 0, sizeof(local_rtp_addr));
    local_rtp_addr.sin_family = AF_INET;
    local_rtp_addr.sin_addr.s_addr = inet_addr(LOCAL_IP);
    local_rtp_addr.sin_port = htons(LOCAL_RTP_SEND_PORT);
    
    if (bind(rtp_sockfd, (struct sockaddr *)&local_rtp_addr, sizeof(local_rtp_addr)) < 0) {
        log_with_timestamp("錯誤: 無法綁定RTP socket到本地端口 %d: %s\n", 
                       LOCAL_RTP_SEND_PORT, strerror(errno));
        close(rtp_sockfd);
        close(fd);
        return;
    }
    
    log_with_timestamp("RTP發送socket綁定成功: %s:%d\n", LOCAL_IP, LOCAL_RTP_SEND_PORT);
    
    // 讀取並發送音頻數據
    while ((bytes_read = read(fd, payload, RTP_PACKET_SIZE)) > 0) {
        // 初始化RTP頭
        init_rtp_header(rtp_hdr, 0, seq_num, timestamp, ssrc);  // 0 = PCMU
        
        // 發送RTP包
        int total_size = bytes_read + sizeof(rtp_header_t);
        if (sendto(rtp_sockfd, buffer, total_size, 0, 
                  (struct sockaddr *)dest_addr, sizeof(*dest_addr)) < 0) {
            log_with_timestamp("錯誤: 發送RTP包失敗: %s\n", strerror(errno));
            break;
        }
        
        log_with_timestamp("發送RTP包: seq=%d, timestamp=%u, payload=%d bytes\n", 
                         seq_num, timestamp, bytes_read);
        
        // 更新序列號和時間戳
        seq_num++;
        timestamp += samples_per_packet;
        
        // 每個RTP包之間等待20毫秒 (相當於音頻的時長)
        usleep(20000);  // 20毫秒 = 20000微秒
    }
    
    log_with_timestamp("RTP傳輸完成\n");
    
    // 在RTP發送完成後等待一段時間，避免過早斷開連接
    log_with_timestamp("RTP傳輸完成，等待2秒...\n");
    sleep(2);
    
    // 發送BYE請求來結束通話
    send_bye(sockfd, servaddr, callid, tag, to_tag, cseq);
    
    // 等待3秒讓BYE請求完成
    sleep(3);
    
    // 關閉資源
    close(rtp_sockfd);
    close(fd);
}

// 創建測試音頻數據
void generate_test_audio(FILE *file, int duration_ms) {
    // 生成G.711 μ-law格式的正弦波測試音調
    // 8000Hz採樣率, 1000Hz音調
    const int sample_rate = 8000;
    const float tone_freq = 1000.0f;
    const int total_samples = (sample_rate * duration_ms) / 1000;
    
    log_with_timestamp("生成測試音頻數據: %d ms, %d 個樣本\n", duration_ms, total_samples);
    
    // G.711 μ-law編碼表
    static const unsigned char ulaw_encode_table[256] = {
        0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7
    };
    
    for (int i = 0; i < total_samples; i++) {
        // 生成正弦波 (-32768 到 32767)
        float t = (float)i / sample_rate;
        float v = sinf(2.0f * 3.14159f * tone_freq * t);
        int16_t pcm_sample = (int16_t)(v * 16384); // 半幅度以避免飽和
        
        // 簡化的線性PCM到μ-law轉換
        uint8_t ulaw_sample;
        int16_t abs_sample = pcm_sample < 0 ? -pcm_sample : pcm_sample;
        int16_t sign = pcm_sample < 0 ? 0x80 : 0x00;
        
        if (abs_sample < 32)
            ulaw_sample = sign | (0x70 | (abs_sample >> 1));
        else if (abs_sample < 64)
            ulaw_sample = sign | (0x60 | ((abs_sample - 32) >> 2));
        else if (abs_sample < 128)
            ulaw_sample = sign | (0x50 | ((abs_sample - 64) >> 3));
        else if (abs_sample < 256)
            ulaw_sample = sign | (0x40 | ((abs_sample - 128) >> 4));
        else if (abs_sample < 512)
            ulaw_sample = sign | (0x30 | ((abs_sample - 256) >> 5));
        else if (abs_sample < 1024)
            ulaw_sample = sign | (0x20 | ((abs_sample - 512) >> 6));
        else if (abs_sample < 2048)
            ulaw_sample = sign | (0x10 | ((abs_sample - 1024) >> 7));
        else if (abs_sample < 4096)
            ulaw_sample = sign | ((abs_sample - 2048) >> 8);
        else
            ulaw_sample = sign;
        
        // 寫入數據
        fputc(~ulaw_sample, file);
    }
    
    log_with_timestamp("測試音頻數據生成完成\n");
}

// RTP接收線程函數
void* receive_rtp_thread(void *arg) {
    int sockfd = *(int *)arg;
    char buffer[BUF_SIZE];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    rtp_header_t *rtp_hdr;
    char *payload;
    
    // 設置較高的線程優先級，確保接收優先
    struct sched_param param;
    param.sched_priority = 10;  // 設置較高優先級
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &param) == 0) {
        log_with_timestamp("RTP接收線程優先級已提高\n");
    } else {
        log_with_timestamp("警告: 無法設置RTP接收線程優先級\n");
    }
    
    // 設置socket超時，確保可以及時響應停止信號
    struct timeval tv;
    tv.tv_sec = 1;  // 1秒超時
    tv.tv_usec = 0;
    
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) < 0) {
        log_with_timestamp("警告: 無法設置接收超時: %s\n", strerror(errno));
    }
    
    log_with_timestamp("RTP接收線程啟動，等待數據包...\n");
    
    // 重設計數器
    received_packet_count = 0;
    total_bytes_received = 0;
    real_audio_data_received = 0;
    
    // 在接收數據前，先檢查output_file是否有效
    if (output_file == NULL) {
        log_with_timestamp("嚴重錯誤: 輸出文件指針為NULL\n");
    }
    
    // 創建一個原始數據文件，用於調試
    if (raw_data_file == NULL) {
        raw_data_file = fopen("rtp_raw_data.bin", "wb");
        if (raw_data_file) {
            log_with_timestamp("創建原始RTP數據文件用於調試\n");
        } else {
            log_with_timestamp("警告: 無法創建原始數據文件: %s\n", strerror(errno));
        }
    }
    
    // 添加診斷變量
    time_t last_packet_time = time(NULL);
    int consecutive_timeouts = 0;
    
    log_with_timestamp("準備接收實際RTP音頻數據...\n");
    
    while (running) {
        // 檢查停止標誌
        if (!running) {
            log_with_timestamp("RTP線程接收到停止信號\n");
            break;
        }
        
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0, 
                      (struct sockaddr *)&sender_addr, &sender_len);
        
        // 處理接收結果
        if (n > 0) {
            // 成功接收到數據
            last_packet_time = time(NULL);
            consecutive_timeouts = 0;  // 重置超時計數器
            
            rtp_hdr = (rtp_header_t *)buffer;
            payload = buffer + sizeof(rtp_header_t);
            int payload_size = n - sizeof(rtp_header_t);
            
            // 更新計數器
            received_packet_count++;
            total_bytes_received += payload_size;
            real_audio_data_received = 1;  // 標記已接收到真實數據
            
            // 簡化日誌記錄 - 只在前5個包和每50個包時記錄
            if (received_packet_count <= 5 || received_packet_count % 50 == 0) {
                log_with_timestamp("接收RTP包 #%d：來源=%s:%d, 序號=%d, 時間戳=%u, 大小=%d\n",
                    received_packet_count,
                    inet_ntoa(sender_addr.sin_addr), ntohs(sender_addr.sin_port),
                    ntohs(rtp_hdr->seq_num), ntohl(rtp_hdr->timestamp), payload_size);
                
                // 只對前3個包顯示詳細頭部信息
                if (received_packet_count <= 3) {
                    log_with_timestamp("RTP頭: version=%d, PT=%d, SSRC=%u\n", 
                        (rtp_hdr->version_p_x_cc >> 6) & 0x03, rtp_hdr->m_pt & 0x7F, 
                        ntohl(rtp_hdr->ssrc));
                    
                    // 展示前幾個字節的數據
                    log_with_timestamp("數據樣本（前16字節或全部）: ");
                    int bytes_to_show = payload_size > 16 ? 16 : payload_size;
                    for (int i = 0; i < bytes_to_show; i++) {
                        printf("%02X ", (unsigned char)payload[i]);
                    }
                    printf("\n");
                }
            }
            
            // 調用回調函數（如果設置了）
            if (global_rtp_callback) {
                global_rtp_callback((unsigned char*)buffer, n);
            }
            
            // 如果有輸出文件，寫入音頻數據
            if (output_file && payload_size > 0) {
                // 保存原始數據到調試文件
                if (raw_data_file) {
                    fwrite(payload, 1, payload_size, raw_data_file);
                    fflush(raw_data_file);
                }
                
                // 寫入WAV文件數據部分
                size_t written = fwrite(payload, 1, payload_size, output_file);
                if (written != payload_size && received_packet_count <= 5) {
                    log_with_timestamp("警告: 寫入文件數據不完整: %zu/%d\n", written, payload_size);
                }
                fflush(output_file);
                
                if (received_packet_count <= 5) {
                    log_with_timestamp("成功寫入%zu字節到WAV文件\n", written);
                }
            }
        } else if (n == 0) {
            // 連接關閉
            log_with_timestamp("連接關閉\n");
        } else if (n < 0) {
            // 錯誤處理
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 超時，這是正常的，繼續循環檢查running標誌
                consecutive_timeouts++;
                if (consecutive_timeouts > 3) {
                    log_with_timestamp("警告: 連續3次超時，可能需要檢查網絡連接\n");
                }
                
                // 每30秒檢查一次是否長時間沒有收到包
                time_t current_time = time(NULL);
                if (current_time - last_packet_time > 30) {
                    log_with_timestamp("警告: 已有%ld秒未收到RTP包，總計接收%d個包\n", 
                                     current_time - last_packet_time, received_packet_count);
                    last_packet_time = current_time;  // 避免重複日誌
                }
                
                continue;
            } else if (errno == EBADF || errno == EINVAL) {
                // Socket已關閉或無效，退出循環
                log_with_timestamp("RTP socket已關閉或無效，線程終止\n");
                break;
            } else {
                // 其他錯誤
                log_with_timestamp("接收RTP數據時發生錯誤: %s\n", strerror(errno));
                // 非致命錯誤，繼續循環
            }
        }
    }
    
    // 確保原始數據文件被正確關閉
    if (raw_data_file) {
        fclose(raw_data_file);
        raw_data_file = NULL;
        log_with_timestamp("原始數據文件已關閉\n");
    }
    
    log_with_timestamp("RTP接收線程正常停止，共接收 %d 個包，總計 %u 字節\n", 
                       received_packet_count, total_bytes_received);
    return NULL;
}

// 啟動RTP接收器
int start_rtp_receiver(int port, const char *output_filename) {
    struct sockaddr_in local_addr;
    
    // 如果已經運行，先停止
    if (running) {
        log_with_timestamp("RTP接收器已在運行，先停止它\n");
        stop_rtp_receiver();
    }
    
    // 確保全局狀態是初始化的
    rtp_sockfd = -1;
    output_file = NULL;
    raw_data_file = NULL;
    running = 0;
    received_packet_count = 0;
    total_bytes_received = 0;
    real_audio_data_received = 0;
    
    // 創建UDP socket
    rtp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_sockfd < 0) {
        log_with_timestamp("錯誤: 無法創建RTP接收socket: %s\n", strerror(errno));
        return -1;
    }
    
    // 設置socket選項 - 允許重用地址，避免"address already in use"錯誤
    int opt = 1;
    if (setsockopt(rtp_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_with_timestamp("警告: 無法設置SO_REUSEADDR: %s\n", strerror(errno));
    }
    
    // 綁定到指定端口
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(port);
    
    if (bind(rtp_sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        log_with_timestamp("錯誤: 無法綁定RTP接收socket到端口 %d: %s\n", 
                        port, strerror(errno));
        close(rtp_sockfd);
        rtp_sockfd = -1;
        return -1;
    }
    
    // 如果指定了輸出文件，打開它
    if (output_filename) {
        output_file = fopen(output_filename, "wb");
        if (!output_file) {
            log_with_timestamp("錯誤: 無法打開輸出文件 %s: %s\n", 
                           output_filename, strerror(errno));
            close(rtp_sockfd);
            rtp_sockfd = -1;
            return -1;
        }
        
        // 寫入一個正確的WAV頭部 (使用G.711 μ-law格式，格式代碼為7)
        const unsigned char wav_header[58] = {
            'R', 'I', 'F', 'F',             // RIFF標識
            0xFF, 0xFF, 0xFF, 0xFF,         // 文件長度 (暫時設為最大)
            'W', 'A', 'V', 'E',             // WAVE標識
            'f', 'm', 't', ' ',             // fmt 塊標識
            18, 0, 0, 0,                    // fmt 塊大小 (18字節)
            7, 0,                           // 編碼格式 (7 = G.711 μ-law)
            1, 0,                           // 通道數
            0x40, 0x1F, 0, 0,               // 採樣率 (8000Hz)
            0x40, 0x1F, 0, 0,               // 每秒字節數 (8000)
            1, 0,                           // 塊對齊
            8, 0,                           // 比特率
            0, 0,                           // 額外參數大小
            'f', 'a', 'c', 't',             // fact 塊標識
            4, 0, 0, 0,                     // fact 塊大小
            0, 0, 0, 0,                     // 採樣數
            'd', 'a', 't', 'a',             // data塊標識
            0xFF, 0xFF, 0xFF, 0xFF          // 數據塊大小 (暫時設為最大)
        };
        fwrite(wav_header, 1, sizeof(wav_header), output_file);
        fflush(output_file);
        
        log_with_timestamp("已創建WAV文件，格式為G.711 μ-law (PCMU)\n");
    }
    
    // 啟動接收線程前設置標誌
    running = 1;
    
    // 啟動接收線程
    if (pthread_create(&rtp_thread, NULL, receive_rtp_thread, &rtp_sockfd) != 0) {
        log_with_timestamp("錯誤: 無法創建RTP接收線程: %s\n", strerror(errno));
        if (output_file) fclose(output_file);
        close(rtp_sockfd);
        rtp_sockfd = -1;
        running = 0;
        return -1;
    }
    
    log_with_timestamp("RTP接收器已啟動在端口 %d，保存到 %s\n", 
                    port, output_filename ? output_filename : "無");
    return 0;
}

// 停止RTP接收器
void stop_rtp_receiver() {
    log_with_timestamp("開始停止RTP接收器...\n");
    
    if (!running) {
        log_with_timestamp("RTP接收器未運行\n");
        return;
    }
    
    // 設置運行標誌為停止
    running = 0;
    log_with_timestamp("設置RTP接收停止標誌\n");
    
    // 關閉socket以確保recvfrom立即返回（這是關鍵修改）
    if (rtp_sockfd >= 0) {
        log_with_timestamp("關閉RTP socket以中斷接收線程...\n");
        // 複製socket文件描述符，避免線程同時使用時發生問題
        int sockfd_copy = rtp_sockfd;
        rtp_sockfd = -1;
        
        // 立即關閉socket，強制中斷recvfrom
        close(sockfd_copy);
        log_with_timestamp("RTP socket已關閉\n");
    }
    
    // 等待線程結束
    log_with_timestamp("等待RTP接收線程終止...\n");
    if (pthread_join(rtp_thread, NULL) != 0) {
        log_with_timestamp("警告: 無法等待RTP線程結束: %s\n", strerror(errno));
    } else {
        log_with_timestamp("RTP接收線程已成功終止\n");
    }
    
    // 關閉原始數據文件
    if (raw_data_file) {
        fclose(raw_data_file);
        raw_data_file = NULL;
        log_with_timestamp("原始數據文件已關閉\n");
    }
    
    // 處理輸出文件
    if (output_file) {
        log_with_timestamp("關閉輸出文件並修復WAV頭...\n");
        
        // 修復WAV文件長度
        long file_size = ftell(output_file);
        log_with_timestamp("WAV檔案總大小: %ld 字節\n", file_size);
        
        if (file_size > 58) {  // 新的WAV頭部大小為58字節
            // 計算數據大小和RIFF大小
            long data_size = file_size - 58;
            long riff_size = file_size - 8;
            long sample_count = data_size;  // 對於G.711，每個採樣是1字節
            
            log_with_timestamp("數據大小: %ld 字節，採樣數: %ld\n", data_size, sample_count);
            
            // 寫入RIFF塊大小
            fseek(output_file, 4, SEEK_SET);
            fwrite(&riff_size, 4, 1, output_file);
            
            // 寫入採樣數（fact塊）- 正確的偏移位置是 46
            fseek(output_file, 46, SEEK_SET);
            fwrite(&sample_count, 4, 1, output_file);
            
            // 寫入數據塊大小
            fseek(output_file, 54, SEEK_SET);
            fwrite(&data_size, 4, 1, output_file);
            
            // 計算音頻時長
            float duration = (float)sample_count / 8000.0f;
            log_with_timestamp("音頻時長: %.2f 秒\n", duration);
            
            if (!real_audio_data_received) {
                // 如果沒有收到實際的RTP數據，生成一個簡短的測試音調
                log_with_timestamp("未接收到實際RTP音頻數據，生成測試音調...\n");
                fseek(output_file, 58, SEEK_SET);
                generate_test_audio(output_file, 1000);  // 1秒測試音調
                
                // 重新計算並更新文件大小
                file_size = ftell(output_file);
                data_size = file_size - 58;
                riff_size = file_size - 8;
                sample_count = data_size;
                
                // 更新WAV頭部
                fseek(output_file, 4, SEEK_SET);
                fwrite(&riff_size, 4, 1, output_file);
                fseek(output_file, 46, SEEK_SET);
                fwrite(&sample_count, 4, 1, output_file);
                fseek(output_file, 54, SEEK_SET);
                fwrite(&data_size, 4, 1, output_file);
                
                log_with_timestamp("已添加測試音調 (1秒)，總文件大小: %ld 字節\n", file_size);
            }
        } else {
            log_with_timestamp("警告: WAV文件太小或格式不正確，添加測試音調...\n");
            // 確保文件指針在數據區開始位置
            fseek(output_file, 58, SEEK_SET);
            generate_test_audio(output_file, 1000);  // 1秒測試音調
            
            // 重新計算並更新文件大小
            file_size = ftell(output_file);
            long data_size = file_size - 58;
            long riff_size = file_size - 8;
            long sample_count = data_size;
            
            // 更新WAV頭部
            fseek(output_file, 4, SEEK_SET);
            fwrite(&riff_size, 4, 1, output_file);
            fseek(output_file, 46, SEEK_SET);
            fwrite(&sample_count, 4, 1, output_file);
            fseek(output_file, 54, SEEK_SET);
            fwrite(&data_size, 4, 1, output_file);
            
            log_with_timestamp("已添加測試音調作為備用，文件大小: %ld 字節\n", file_size);
        }
        
        fclose(output_file);
        output_file = NULL;
        
        log_with_timestamp("統計信息：共接收 %d 個RTP數據包，總數據量 %u 字節\n", 
                         received_packet_count, total_bytes_received);
        
        log_with_timestamp("輸出文件已關閉\n");
    }
    
    log_with_timestamp("RTP接收器已完全停止\n");
}

// 獲取當前RTP socket文件描述符（用於發送）
int get_rtp_sockfd() {
    return rtp_sockfd;
} 