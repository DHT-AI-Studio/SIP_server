#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include "lib/sip_client.h"

// WebSocket 服務端配置
#define WS_PORT 8080
#define MAX_PAYLOAD 4096
#define RTP_LISTEN_TIMEOUT 300  // 通話最長持續時間（秒）
#define WAV_FILE_PATH "output_ulaw.wav"  // 默認播放的WAV文件路徑

// 全局變量
static struct lws_context *context;
static struct lws *client_wsi = NULL;
static volatile int force_exit = 0;
static pthread_t sip_thread;
static volatile int sip_call_active = 0;
static sip_session_t session;
static volatile int rtp_packets_received = 0;
static volatile int rtp_processing_paused = 0;  // 新增：控制RTP處理的標誌

// 自定義 RTP 處理回調函數的聲明
void custom_rtp_callback(const unsigned char *rtp_data, size_t data_size);

// RTP 音頻傳送線程
void* rtp_audio_thread(void *arg) {
    void **args = (void**)arg;
    int sockfd = *((int*)args[0]);
    struct sockaddr_in *dest_addr = (struct sockaddr_in*)args[1];
    const char *wav_file = (const char*)args[2];
    int dest_port = *((int*)args[3]);
    const char *callid = (const char*)args[4];
    const char *tag = (const char*)args[5];
    const char *to_tag = (const char*)args[6];
    const char *cseq = (const char*)args[7];
    struct sockaddr_in *servaddr = (struct sockaddr_in*)args[8];
    int shared_rtp_sockfd = *((int*)args[9]);  // 共享的RTP socket
    
    log_with_timestamp("RTP 音頻傳送線程啟動\n");
    
    // 設置較低的線程優先級，確保接收線程優先
    struct sched_param param;
    param.sched_priority = 1;  // 設置較低優先級
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &param) == 0) {
        log_with_timestamp("RTP發送線程優先級已降低\n");
    } else {
        log_with_timestamp("警告: 無法設置RTP發送線程優先級\n");
    }
    
    // 添加10秒延遲，讓通話先進行純接收模式
    log_with_timestamp("等待10秒後開始播放音檔，測試純接收RTP模式...\n");
    for (int i = 10; i > 0; i--) {
        log_with_timestamp("音檔播放倒計時: %d 秒...\n", i);
        sleep(1);
        
        // 檢查通話是否還活躍
        if (!sip_call_active) {
            log_with_timestamp("通話已結束，取消音檔播放\n");
            goto cleanup;
        }
    }
    
    log_with_timestamp("10秒純接收測試完成，現在準備播放音檔\n");
    
    // **進程分離方案**: 使用子進程處理RTP發送，避免與接收線程競爭資源
    log_with_timestamp("使用子進程分離RTP發送，避免資源競爭...\n");
    
    pid_t audio_pid = fork();
    
    if (audio_pid == 0) {
        // 子進程：專門處理RTP發送
        log_with_timestamp("RTP發送子進程啟動，PID: %d\n", getpid());
        
        // 額外延遲，確保父進程接收狀態穩定
        log_with_timestamp("子進程：再等待5秒，確保父進程接收狀態穩定...\n");
        sleep(5);
        
        // **調試選項**: 純模擬測試，不實際發送RTP
        int simulate_only = 0;  // 設置為1則只模擬，不實際發送
        if (simulate_only) {
            log_with_timestamp("子進程：**純模擬模式** - 不實際發送RTP，只模擬時間\n");
            // 模擬音頻播放的時間（約12秒）
            for (int i = 0; i < 600; i++) {
                usleep(20000);  // 20ms間隔
                if (i % 200 == 0) {
                    log_with_timestamp("子進程：模擬發送 %d 個包（實際未發送）\n", i);
                }
            }
            log_with_timestamp("子進程：模擬播放完成，進程結束\n");
            exit(0);
        }
        
        // 創建獨立的發送socket
        int rtp_sockfd = shared_rtp_sockfd;  // 直接使用共享的RTP socket
        if (rtp_sockfd < 0) {
            log_with_timestamp("子進程：無效的共享RTP socket: %d\n", rtp_sockfd);
            exit(1);
        }
        
        log_with_timestamp("子進程：使用共享RTP socket %d，無需重新綁定端口\n", rtp_sockfd);
        
        // 打開WAV文件
        FILE *wav_fp = fopen(wav_file, "rb");
        if (!wav_fp) {
            log_with_timestamp("子進程：無法打開 WAV 文件: %s\n", strerror(errno));
            close(rtp_sockfd);
            exit(1);
        }
        
        // 跳過WAV文件頭
        fseek(wav_fp, 64, SEEK_SET);
        
        // 設置目標地址 - 使用SIP協商確定的端口
        dest_addr->sin_port = htons(dest_port);  // 直接使用協商的端口
        
        log_with_timestamp("子進程：RTP發送目標端口: %d（SIP協商確定）\n", dest_port);
        
        // 準備RTP包
        unsigned char rtp_packet[172];
        rtp_header_t *rtp_hdr = (rtp_header_t *)rtp_packet;
        unsigned char *payload = rtp_packet + sizeof(rtp_header_t);
        
        unsigned short seq_num = 0;
        unsigned int timestamp = 0;
        unsigned int ssrc = rand();
        
        log_with_timestamp("子進程：開始RTP音頻發送到 %s:%d\n", 
                          inet_ntoa(dest_addr->sin_addr), dest_port);
        
        int total_packets_sent = 0;
        size_t bytes_read;
        
        // 正常連續播放
        while ((bytes_read = fread(payload, 1, 160, wav_fp)) > 0) {
            // 初始化RTP頭
            memset(rtp_hdr, 0, sizeof(rtp_header_t));
            init_rtp_header(rtp_hdr, 0, seq_num, timestamp, ssrc);
            
            // 發送RTP包
            int packet_size = sizeof(rtp_header_t) + bytes_read;
            ssize_t sent = sendto(rtp_sockfd, rtp_packet, packet_size, 0,
                                 (struct sockaddr *)dest_addr, sizeof(*dest_addr));
            
            if (sent > 0) {
                total_packets_sent++;
                seq_num++;
                timestamp += bytes_read;
                
                // 減少日誌頻率
                if (total_packets_sent % 200 == 0) {
                    log_with_timestamp("子進程：已發送 %d 個RTP包\n", total_packets_sent);
                }
                
                // 標準G.711間隔
                usleep(20000);
                
            } else {
                log_with_timestamp("子進程：發送RTP包失敗: %s\n", strerror(errno));
                break;
            }
        }
        
        log_with_timestamp("子進程：音檔播放完成，總共發送 %d 個RTP包\n", total_packets_sent);
        
        fclose(wav_fp);
        // 不關閉socket，因為它是與父進程共享的
        // close(rtp_sockfd);  
        
        log_with_timestamp("子進程：RTP發送完成，進程結束（共享socket未關閉）\n");
        exit(0);  // 子進程正常結束
        
    } else if (audio_pid > 0) {
        // 父進程：繼續接收RTP，同時子進程發送音頻
        log_with_timestamp("父進程：RTP發送子進程已啟動 (PID: %d)\n", audio_pid);
        log_with_timestamp("**關鍵**: 父子進程共享RTP socket，實現真正的雙向通話\n");
        
        // 父進程監控RTP接收狀態，檢測何時中斷
        log_with_timestamp("父進程：繼續接收RTP，同時監控接收狀態...\n");
        int last_count = rtp_packets_received;
        int monitoring_seconds = 0;
        
        // 監控60秒或直到通話結束
        while (sip_call_active && monitoring_seconds < 60) {
            sleep(2);  // 每2秒檢查一次
            monitoring_seconds += 2;
            
            int current_count = rtp_packets_received;
            int received_in_period = current_count - last_count;
            
            log_with_timestamp("父進程監控 %ds: 新收到 %d 個RTP包（總計: %d）\n", 
                             monitoring_seconds, received_in_period, current_count);
            
            if (received_in_period == 0 && monitoring_seconds > 10) {
                log_with_timestamp("警告: %d秒內未收到新的RTP包\n", monitoring_seconds);
            }
            
            last_count = current_count;
        }
        
        // 等待子進程完成音頻發送
        int status;
        log_with_timestamp("父進程：等待子進程完成音頻發送...\n");
        waitpid(audio_pid, &status, 0);
        
        if (WIFEXITED(status)) {
            log_with_timestamp("父進程：子進程正常結束，退出碼: %d\n", WEXITSTATUS(status));
        } else {
            log_with_timestamp("父進程：子進程異常結束\n");
        }
        
        log_with_timestamp("父進程：子進程結束，繼續正常的RTP接收...\n");
        
    } else {
        // fork失敗
        log_with_timestamp("錯誤：無法創建RTP發送子進程: %s\n", strerror(errno));
    }
    
cleanup:
    // 釋放參數記憶體
    for (int i = 0; i < 10; i++) {
        if (args[i] && i != 2) { // 不釋放 wav_file
            free(args[i]);
        }
    }
    free(args);
    
    log_with_timestamp("RTP 音頻傳送線程結束\n");
    return NULL;
}

// SIP 通話線程函數
void* sip_call_thread(void* arg) {
    const char *callee = (const char*)arg;
    int rtp_timeout_counter = 0;
    pid_t audio_pid = -1;
    int audio_process_created = 0;
    pthread_t audio_thread;
    
    log_with_timestamp("SIP 線程啟動，準備撥打電話到 %s\n", callee);
    
    // 初始化 SIP 會話
    if (init_sip_session(&session) != 0) {
        log_with_timestamp("初始化 SIP 會話失敗\n");
        sip_call_active = 0;
        free((void*)arg); // 釋放 callee_copy
        return NULL;
    }
    
    // 發起 SIP 呼叫
    if (make_sip_call(&session, callee) != 0) {
        log_with_timestamp("SIP 呼叫失敗\n");
        close_sip_session(&session);
        sip_call_active = 0;
        free((void*)arg); // 釋放 callee_copy
        return NULL;
    }
    
    log_with_timestamp("SIP 呼叫成功建立\n");
    
    // 創建 RTP 目標地址
    struct sockaddr_in rtp_dest_addr;
    memset(&rtp_dest_addr, 0, sizeof(rtp_dest_addr));
    rtp_dest_addr.sin_family = AF_INET;
    rtp_dest_addr.sin_addr.s_addr = inet_addr(SIP_SERVER);
    
    // 使用對方在SIP回應中指定的RTP端口
    rtp_dest_addr.sin_port = htons(session.remote_rtp_port);
    
    log_with_timestamp("對方 RTP 端口: %d，我方 RTP 接收端口: %d\n", 
                      session.remote_rtp_port, LOCAL_RTP_PORT);
    
    // **關鍵修復**: 正確的RTP端口配置
    // 我們應該監聽自己在SDP中宣告的端口，而不是對方的端口
    int our_rtp_port = LOCAL_RTP_PORT;  // 我們自己的端口，在SDP中已宣告
    int their_rtp_port = session.remote_rtp_port;  // 對方的端口，從SDP中解析
    
    log_with_timestamp("**正確配置**: 我方監聽端口 %d，對方監聽端口 %d\n", 
                      our_rtp_port, their_rtp_port);
    
    // 設置RTP回調函數
    set_rtp_callback(custom_rtp_callback);
    
    // 啟動 RTP 接收器來接收對方的音頻
    log_with_timestamp("啟動 RTP 接收器...\n");
    start_rtp_receiver(our_rtp_port, "received_from_server.wav");
    
    // 檢查 WAV 文件是否存在並啟動音頻播放進程
    if (access(WAV_FILE_PATH, F_OK) == 0) {
        log_with_timestamp("準備播放 WAV 文件: %s\n", WAV_FILE_PATH);
        
        // 創建音頻播放線程（內部會使用fork創建子進程）
        void **audio_args = (void**)malloc(10 * sizeof(void*));
        audio_args[0] = malloc(sizeof(int));
        *((int*)audio_args[0]) = session.sockfd;
        
        audio_args[1] = malloc(sizeof(struct sockaddr_in));
        memcpy(audio_args[1], &rtp_dest_addr, sizeof(struct sockaddr_in));
        
        audio_args[2] = (void*)WAV_FILE_PATH;  // 常量字符串
        
        audio_args[3] = malloc(sizeof(int));
        *((int*)audio_args[3]) = their_rtp_port;
        
        audio_args[4] = strdup(session.callid);
        audio_args[5] = strdup(session.tag);
        audio_args[6] = strdup(session.to_tag);
        audio_args[7] = strdup(session.cseq);
        
        audio_args[8] = malloc(sizeof(struct sockaddr_in));
        memcpy(audio_args[8], &session.servaddr, sizeof(struct sockaddr_in));
        
        // 新增：傳遞RTP socket文件描述符
        audio_args[9] = malloc(sizeof(int));
        int current_rtp_sockfd = get_rtp_sockfd();  // 獲取當前RTP socket
        *((int*)audio_args[9]) = current_rtp_sockfd;  // 傳遞RTP socket給子進程
        
        // 創建音頻處理線程（線程內部會fork子進程）
        if (pthread_create(&audio_thread, NULL, rtp_audio_thread, audio_args) == 0) {
            log_with_timestamp("音頻處理線程已創建（將使用子進程發送RTP）\n");
            audio_process_created = 1;
            audio_pid = 0; // 標記有音頻進程
        } else {
            log_with_timestamp("創建音頻處理線程失敗\n");
            // 釋放參數記憶體
            for (int i = 0; i < 10; i++) {
                if (audio_args[i] && i != 2) {
                    free(audio_args[i]);
                }
            }
            free(audio_args);
        }
    } else {
        log_with_timestamp("錯誤: WAV 文件 %s 不存在: %s\n", WAV_FILE_PATH, strerror(errno));
    }
    
    // 持續監聽 RTP 封包，不立即掛斷
    log_with_timestamp("保持通話並監聽 RTP 封包，最多 %d 秒...\n", RTP_LISTEN_TIMEOUT);
    
    // 等待掛斷請求或超時
    while (sip_call_active && rtp_timeout_counter < RTP_LISTEN_TIMEOUT) {
        sleep(1);
        rtp_timeout_counter++;
        
        // 每 10 秒顯示一次通話統計
        if (rtp_timeout_counter % 10 == 0) {
            log_with_timestamp("通話持續 %d 秒，已接收 %d 個 RTP 封包\n", 
                              rtp_timeout_counter, rtp_packets_received);
        }
        
        // 檢查音頻線程是否還在運行
        if (audio_process_created && rtp_timeout_counter % 5 == 0) {
            // 每5秒檢查一次音頻線程狀態，但不強制結束通話
            log_with_timestamp("檢查音頻線程狀態...\n");
        }
    }
    
    log_with_timestamp("通話循環結束，準備清理資源\n");

    // 等待音頻線程結束（如果它被創建了）
    if (audio_process_created) {
        log_with_timestamp("等待音頻處理線程結束...\n");
        pthread_join(audio_thread, NULL);
        log_with_timestamp("音頻處理線程已結束\n");
    }
    
    // 停止 RTP 接收和清除回調
    log_with_timestamp("停止 RTP 接收...\n");
    clear_rtp_callback();
    stop_rtp_receiver();
    
    // 發送 BYE 結束通話
    log_with_timestamp("發送 BYE 結束通話\n");
    send_bye(session.sockfd, &session.servaddr, session.callid, 
             session.tag, session.to_tag, session.cseq);
    
    // 關閉 SIP 會話
    close_sip_session(&session);
    
    sip_call_active = 0;
    rtp_packets_received = 0;
    log_with_timestamp("SIP 通話結束\n");
    
    free((void*)arg); // 釋放 callee_copy
    return NULL;
}

// WebSocket 回調函數
static int callback_http(struct lws *wsi, enum lws_callback_reasons reason,
                        void *user, void *in, size_t len) {
    
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            log_with_timestamp("WebSocket 連接建立\n");
            client_wsi = wsi;
            break;
            
        case LWS_CALLBACK_RECEIVE:
            log_with_timestamp("收到 WebSocket 消息: %.*s\n", (int)len, (char*)in);
            
            // 解析客戶端請求
            if (strncmp((char*)in, "CALL:", 5) == 0) {
                // 複製被叫號碼，確保以 null 結尾
                char *raw_callee = (char*)in + 5;
                char callee[64] = {0};  // 避免緩衝區溢出
                
                // 安全地複製並確保字符串結束
                size_t callee_len = len - 5 < sizeof(callee) - 1 ? len - 5 : sizeof(callee) - 1;
                strncpy(callee, raw_callee, callee_len);
                callee[callee_len] = '\0';
                
                // 移除換行符和任何非數字字符
                for (int i = 0; i < callee_len; i++) {
                    if (callee[i] == '\n' || callee[i] == '\r') {
                        callee[i] = '\0';
                        break;
                    }
                }
                
                log_with_timestamp("收到打電話請求，目標號碼: %s\n", callee);
                
                if (!sip_call_active) {
                    sip_call_active = 1;
                    rtp_packets_received = 0;
                    
                    // 創建 SIP 線程
                    char *callee_copy = strdup(callee);
                    if (pthread_create(&sip_thread, NULL, sip_call_thread, callee_copy) != 0) {
                        log_with_timestamp("創建 SIP 線程失敗\n");
                        sip_call_active = 0;
                        free(callee_copy);
                    }
                } else {
                    log_with_timestamp("已有通話進行中，忽略新的通話請求\n");
                }
            }
            else if (strncmp((char*)in, "HANGUP", 6) == 0) {
                log_with_timestamp("收到掛斷請求\n");
                sip_call_active = 0;
            }
            break;
            
        case LWS_CALLBACK_CLOSED:
            log_with_timestamp("WebSocket 連接關閉\n");
            client_wsi = NULL;
            sip_call_active = 0;
            break;
            
        default:
            break;
    }
    
    return 0;
}

// WebSocket 協議定義
static struct lws_protocols protocols[] = {
    {
        "sip-demo-protocol",
        callback_http,
        0,
        MAX_PAYLOAD,
    },
    { NULL, NULL, 0, 0 } // 結束標記
};

// 信號處理函數
void sigint_handler(int sig) {
    log_with_timestamp("收到中斷信號，正在關閉服務器...\n");
    force_exit = 1;
    sip_call_active = 0;
    lws_cancel_service(context);
}

// 發送 RTP 數據到 WebSocket 客戶端
void send_rtp_to_client(const unsigned char *rtp_data, size_t data_size) {
    if (client_wsi) {
        unsigned char buf[LWS_PRE + MAX_PAYLOAD];
        unsigned char *p = &buf[LWS_PRE];
        
        // 準備消息：RTP: 前綴 + base64 編碼的 RTP 數據
        strcpy((char*)p, "RTP:");
        p += 4;
        
        // 簡單的十六進制編碼 (為了演示，實際應用中應該使用 base64)
        for (size_t i = 0; i < data_size && (p - &buf[LWS_PRE]) < MAX_PAYLOAD - 10; i++) {
            sprintf((char*)p, "%02X", rtp_data[i]);
            p += 2;
        }
        
        size_t msg_len = p - &buf[LWS_PRE];
        
        if (lws_write(client_wsi, &buf[LWS_PRE], msg_len, LWS_WRITE_TEXT) < 0) {
            log_with_timestamp("發送 RTP 數據到客戶端失敗\n");
        }
    }
}

// 自定義 RTP 數據處理回調函數
void custom_rtp_callback(const unsigned char *rtp_data, size_t data_size) {
    rtp_packets_received++;
    
    // 檢查是否暫停處理
    if (rtp_processing_paused) {
        // 播放期間暫停處理，但仍然計數以保持統計
        if (rtp_packets_received % 100 == 0) {
            log_with_timestamp("音檔播放期間：接收但暫停處理RTP包 #%d（大小: %zu字節）\n", 
                             rtp_packets_received, data_size);
        }
        return;  // 直接返回，不處理數據
    }
    
    // 正常處理RTP數據
    // 詳細記錄 RTP 包信息
    if (rtp_packets_received <= 5 || rtp_packets_received % 50 == 0) {
        log_with_timestamp("接收到 RTP 封包 #%d，大小: %zu 字節\n", rtp_packets_received, data_size);
        
        // 顯示前 20 個字節的十六進制表示，有助於調試
        if (data_size > 0 && rtp_packets_received <= 3) {
            printf("  RTP 數據前12字節: ");
            for (int i = 0; i < 12 && i < data_size; i++) {
                printf("%02X ", rtp_data[i]);
            }
            printf("\n");
        }
    }
    
    // 發送到 WebSocket 客戶端
    send_rtp_to_client(rtp_data, data_size);
}

int main(void) {
    struct lws_context_creation_info info;
    
    log_with_timestamp("WebSocket SIP Demo 服務器啟動\n");
    
    // 設置信號處理
    signal(SIGINT, sigint_handler);
    
    // 初始化 libwebsockets
    memset(&info, 0, sizeof(info));
    info.port = WS_PORT;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.iface = NULL;  // 綁定到所有網路介面，使其可以接受外部連接
    info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
    
    context = lws_create_context(&info);
    if (!context) {
        log_with_timestamp("創建 WebSocket 上下文失敗\n");
        return -1;
    }
    
    log_with_timestamp("WebSocket 服務器監聽所有網路介面上的端口 %d\n", WS_PORT);
    
    // 主循環
    while (!force_exit) {
        lws_service(context, 50);
    }
    
    // 清理
    if (sip_call_active) {
        sip_call_active = 0;
        pthread_join(sip_thread, NULL);
    }
    
    lws_context_destroy(context);
    log_with_timestamp("WebSocket 服務器已關閉\n");
    
    return 0;
} 