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
#include <stdint.h>
#include "lib/sip_client.h"

// WebSocket 服務端配置
#define WS_PORT 8080
#define MAX_PAYLOAD (200 * 1024)  // 200KB，足夠處理大部分 WAV 檔案
#define RTP_LISTEN_TIMEOUT 300  // 通話最長持續時間（秒）
#define MAX_FILE_SIZE (1024 * 1024)  // 最大 1MB WAV 檔案
#define UPLOAD_DIR "uploaded_wavs"  // 上傳檔案目錄

// 全局變量
static struct lws_context *context;
static struct lws *client_wsi = NULL;
static volatile int force_exit = 0;
static pthread_t sip_thread;

// 消息緩衝區用於處理分片消息
typedef struct {
    char *buffer;
    size_t size;
    size_t capacity;
    int is_receiving;
} message_buffer_t;

static message_buffer_t msg_buffer = {NULL, 0, 0, 0};
static volatile int sip_call_active = 0;
static sip_session_t session;
static volatile int rtp_packets_received = 0;

// Base64 解碼表
static const unsigned char base64_decode_table[256] = {
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
    64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
    64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};

// 自定義 RTP 處理回調函數的聲明
void custom_rtp_callback(const unsigned char *rtp_data, size_t data_size);

// Base64 解碼函數
unsigned char* base64_decode(const char* encoded_data, size_t input_length, size_t *output_length) {
    if (input_length % 4 != 0) return NULL;
    
    *output_length = input_length / 4 * 3;
    if (encoded_data[input_length - 1] == '=') (*output_length)--;
    if (encoded_data[input_length - 2] == '=') (*output_length)--;
    
    unsigned char *decoded_data = malloc(*output_length);
    if (decoded_data == NULL) return NULL;
    
    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t sextet_a = encoded_data[i] == '=' ? 0 & i++ : base64_decode_table[encoded_data[i++]];
        uint32_t sextet_b = encoded_data[i] == '=' ? 0 & i++ : base64_decode_table[encoded_data[i++]];
        uint32_t sextet_c = encoded_data[i] == '=' ? 0 & i++ : base64_decode_table[encoded_data[i++]];
        uint32_t sextet_d = encoded_data[i] == '=' ? 0 & i++ : base64_decode_table[encoded_data[i++]];
        
        uint32_t triple = (sextet_a << 3 * 6) + (sextet_b << 2 * 6) + (sextet_c << 1 * 6) + (sextet_d << 0 * 6);
        
        if (j < *output_length) decoded_data[j++] = (triple >> 2 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 1 * 8) & 0xFF;
        if (j < *output_length) decoded_data[j++] = (triple >> 0 * 8) & 0xFF;
    }
    
    return decoded_data;
}

// 創建上傳目錄
void ensure_upload_directory() {
    struct stat st = {0};
    if (stat(UPLOAD_DIR, &st) == -1) {
        if (mkdir(UPLOAD_DIR, 0755) == 0) {
            log_with_timestamp("創建上傳目錄: %s\n", UPLOAD_DIR);
        } else {
            log_with_timestamp("創建上傳目錄失敗: %s\n", strerror(errno));
        }
    }
}

// 保存上傳的 WAV 檔案
int save_uploaded_wav(const char *filename, const unsigned char *data, size_t size) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", UPLOAD_DIR, filename);
    
    // 檢查檔案大小
    if (size > MAX_FILE_SIZE) {
        log_with_timestamp("上傳檔案太大: %zu 字節，最大允許 %d 字節\n", size, MAX_FILE_SIZE);
        return -1;
    }
    
    // 保存檔案
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        log_with_timestamp("無法創建檔案 %s: %s\n", filepath, strerror(errno));
        return -1;
    }
    
    size_t written = fwrite(data, 1, size, fp);
    fclose(fp);
    
    if (written != size) {
        log_with_timestamp("寫入檔案失敗: 期望 %zu 字節，實際 %zu 字節\n", size, written);
        return -1;
    }
    
    log_with_timestamp("成功保存上傳檔案: %s (%zu 字節)\n", filepath, size);
    return 0;
}

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
    
    log_with_timestamp("RTP 音頻傳送線程啟動，播放檔案: %s\n", wav_file);
    
    // 設置較低的線程優先級，確保接收線程優先
    struct sched_param param;
    param.sched_priority = 1;  // 設置較低優先級
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &param) == 0) {
        log_with_timestamp("RTP發送線程優先級已降低\n");
    } else {
        log_with_timestamp("警告: 無法設置RTP發送線程優先級\n");
    }
    
    // **進程分離方案**: 使用子進程處理RTP發送，避免與接收線程競爭資源
    log_with_timestamp("使用子進程分離RTP發送，避免資源競爭...\n");
    
    pid_t audio_pid = fork();
    
    if (audio_pid == 0) {
        // 子進程：專門處理RTP發送
        log_with_timestamp("RTP發送子進程啟動，PID: %d\n", getpid());
        
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

// 播放指定的 WAV 檔案
int play_wav_file(const char *filename) {
    if (!sip_call_active) {
        log_with_timestamp("沒有活躍的通話，無法播放音頻\n");
        return -1;
    }
    
    // 檢查檔案是否存在
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", UPLOAD_DIR, filename);
    
    if (access(filepath, F_OK) != 0) {
        log_with_timestamp("檔案不存在: %s\n", filepath);
        return -1;
    }
    
    log_with_timestamp("開始播放 WAV 檔案: %s\n", filepath);
    
    // 創建 RTP 目標地址
    struct sockaddr_in rtp_dest_addr;
    memset(&rtp_dest_addr, 0, sizeof(rtp_dest_addr));
    rtp_dest_addr.sin_family = AF_INET;
    rtp_dest_addr.sin_addr.s_addr = inet_addr(SIP_SERVER);
    
    // 使用對方在SIP回應中指定的RTP端口
    rtp_dest_addr.sin_port = htons(session.remote_rtp_port);
    
    // 創建音頻播放線程（內部會使用fork創建子進程）
    void **audio_args = (void**)malloc(10 * sizeof(void*));
    audio_args[0] = malloc(sizeof(int));
    *((int*)audio_args[0]) = session.sockfd;
    
    audio_args[1] = malloc(sizeof(struct sockaddr_in));
    memcpy(audio_args[1], &rtp_dest_addr, sizeof(struct sockaddr_in));
    
    audio_args[2] = strdup(filepath);  // 使用完整路徑
    
    audio_args[3] = malloc(sizeof(int));
    *((int*)audio_args[3]) = session.remote_rtp_port;
    
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
    pthread_t audio_thread;
    if (pthread_create(&audio_thread, NULL, rtp_audio_thread, audio_args) == 0) {
        log_with_timestamp("音頻處理線程已創建，正在播放: %s\n", filename);
        
        // 分離線程，讓它在背景中執行
        pthread_detach(audio_thread);
        
        return 0;
    } else {
        log_with_timestamp("創建音頻處理線程失敗\n");
        // 釋放參數記憶體
        for (int i = 0; i < 10; i++) {
            if (audio_args[i]) {
                free(audio_args[i]);
            }
        }
        free(audio_args);
        return -1;
    }
}

// SIP 通話線程函數
void* sip_call_thread(void* arg) {
    const char *callee = (const char*)arg;
    int rtp_timeout_counter = 0;
    
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
    
    // 使用對方在SIP回應中指定的RTP端口
    int our_rtp_port = LOCAL_RTP_PORT;  // 我們自己的端口，在SDP中已宣告
    int their_rtp_port = session.remote_rtp_port;  // 對方的端口，從SDP中解析
    
    log_with_timestamp("**正確配置**: 我方監聽端口 %d，對方監聽端口 %d\n", 
                      our_rtp_port, their_rtp_port);
    
    // 設置RTP回調函數
    set_rtp_callback(custom_rtp_callback);
    
    // 啟動 RTP 接收器來接收對方的音頻
    log_with_timestamp("啟動 RTP 接收器...\n");
    start_rtp_receiver(our_rtp_port, "received_from_server.wav");
    
    // 不自動播放檔案，等待客戶端指令
    log_with_timestamp("通話建立完成，等待客戶端指令播放音頻檔案\n");
    
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
    }
    
    log_with_timestamp("通話循環結束，準備清理資源\n");
    
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
            {
                // 處理分片消息
                int is_final = lws_is_final_fragment(wsi);
                int is_first = lws_is_first_fragment(wsi);
                
                // 如果是第一個片段，初始化緩衝區
                if (is_first || !msg_buffer.is_receiving) {
                    if (msg_buffer.buffer) {
                        free(msg_buffer.buffer);
                    }
                    msg_buffer.capacity = MAX_PAYLOAD;
                    msg_buffer.buffer = malloc(msg_buffer.capacity);
                    msg_buffer.size = 0;
                    msg_buffer.is_receiving = 1;
                    
                    if (!msg_buffer.buffer) {
                        log_with_timestamp("無法分配消息緩衝區\n");
                        return -1;
                    }
                }
                
                // 檢查緩衝區空間
                if (msg_buffer.size + len > msg_buffer.capacity) {
                    size_t new_capacity = msg_buffer.capacity * 2;
                    while (new_capacity < msg_buffer.size + len) {
                        new_capacity *= 2;
                    }
                    
                    char *new_buffer = realloc(msg_buffer.buffer, new_capacity);
                    if (!new_buffer) {
                        log_with_timestamp("無法擴展消息緩衝區\n");
                        return -1;
                    }
                    
                    msg_buffer.buffer = new_buffer;
                    msg_buffer.capacity = new_capacity;
                }
                
                // 添加數據到緩衝區
                memcpy(msg_buffer.buffer + msg_buffer.size, in, len);
                msg_buffer.size += len;
                
                // 如果不是最後一個片段，繼續等待
                if (!is_final) {
                    return 0;
                }
                
                // 處理完整消息
                msg_buffer.is_receiving = 0;
                
                // 只記錄消息的開頭部分
                if (msg_buffer.size > 100) {
                    log_with_timestamp("收到 WebSocket 消息 (%zu 字節): %.100s...\n", 
                                      msg_buffer.size, msg_buffer.buffer);
                } else {
                    log_with_timestamp("收到 WebSocket 消息: %.*s\n", 
                                      (int)msg_buffer.size, msg_buffer.buffer);
                }
                
                // 使用完整消息進行處理
                char *full_msg = msg_buffer.buffer;
                size_t full_len = msg_buffer.size;
            
            // 解析客戶端請求
            if (strncmp(full_msg, "CALL:", 5) == 0) {
                // 複製被叫號碼，確保以 null 結尾
                char *raw_callee = full_msg + 5;
                char callee[64] = {0};  // 避免緩衝區溢出
                
                // 安全地複製並確保字符串結束
                size_t callee_len = full_len - 5 < sizeof(callee) - 1 ? full_len - 5 : sizeof(callee) - 1;
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
            else if (strncmp(full_msg, "HANGUP", 6) == 0) {
                log_with_timestamp("收到掛斷請求\n");
                sip_call_active = 0;
            }
            else if (strncmp(full_msg, "WAV_UPLOAD:", 11) == 0) {
                // 處理 WAV 檔案上傳
                char *upload_data = full_msg + 11;
                char *filename_end = strchr(upload_data, ':');
                
                if (filename_end) {
                    // 提取檔案名稱
                    size_t filename_len = filename_end - upload_data;
                    char filename[256] = {0};
                    if (filename_len < sizeof(filename)) {
                        strncpy(filename, upload_data, filename_len);
                        filename[filename_len] = '\0';
                        
                        // 提取 Base64 編碼的數據
                        char *encoded_data = filename_end + 1;
                        size_t encoded_len = full_len - 11 - filename_len - 1;
                        
                        log_with_timestamp("收到 WAV 檔案上傳: %s (編碼大小: %zu 字節)\n", 
                                          filename, encoded_len);
                        
                        // Base64 解碼
                        size_t decoded_len;
                        unsigned char *decoded_data = base64_decode(encoded_data, encoded_len, &decoded_len);
                        
                        if (decoded_data) {
                            // 保存檔案
                            if (save_uploaded_wav(filename, decoded_data, decoded_len) == 0) {
                                // 發送確認消息
                                char ack_msg[300];
                                snprintf(ack_msg, sizeof(ack_msg), "WAV_ACK:檔案 %s 上傳成功 (%zu 字節)", 
                                        filename, decoded_len);
                                
                                if (client_wsi) {
                                    unsigned char buf[LWS_PRE + MAX_PAYLOAD];
                                    unsigned char *p = &buf[LWS_PRE];
                                    
                                    size_t msg_len = strlen(ack_msg);
                                    if (msg_len > MAX_PAYLOAD - 1) {
                                        msg_len = MAX_PAYLOAD - 1;
                                    }
                                    
                                    memcpy(p, ack_msg, msg_len);
                                    lws_write(client_wsi, p, msg_len, LWS_WRITE_TEXT);
                                }
                            }
                            
                            free(decoded_data);
                        } else {
                            log_with_timestamp("Base64 解碼失敗\n");
                        }
                    } else {
                        log_with_timestamp("檔案名稱太長\n");
                    }
                } else {
                    log_with_timestamp("無效的上傳格式\n");
                }
            }
            else if (strncmp(full_msg, "PLAY_WAV:", 9) == 0) {
                // 處理播放 WAV 檔案請求
                char *filename = full_msg + 9;
                size_t filename_len = full_len - 9;
                
                // 創建以 null 結尾的檔案名稱
                char wav_filename[256] = {0};
                if (filename_len < sizeof(wav_filename)) {
                    strncpy(wav_filename, filename, filename_len);
                    wav_filename[filename_len] = '\0';
                    
                    // 移除可能的換行符
                    for (int i = 0; i < filename_len; i++) {
                        if (wav_filename[i] == '\n' || wav_filename[i] == '\r') {
                            wav_filename[i] = '\0';
                            break;
                        }
                    }
                    
                    log_with_timestamp("收到播放 WAV 檔案請求: %s\n", wav_filename);
                    
                    if (play_wav_file(wav_filename) == 0) {
                        // 發送確認消息
                        char ack_msg[300];
                        snprintf(ack_msg, sizeof(ack_msg), "WAV_ACK:開始播放檔案 %s", wav_filename);
                        
                        if (client_wsi) {
                            unsigned char buf[LWS_PRE + MAX_PAYLOAD];
                            unsigned char *p = &buf[LWS_PRE];
                            
                            size_t msg_len = strlen(ack_msg);
                            if (msg_len > MAX_PAYLOAD - 1) {
                                msg_len = MAX_PAYLOAD - 1;
                            }
                            
                            memcpy(p, ack_msg, msg_len);
                            lws_write(client_wsi, p, msg_len, LWS_WRITE_TEXT);
                        }
                    } else {
                        // 發送錯誤消息
                        char error_msg[300];
                        snprintf(error_msg, sizeof(error_msg), "WAV_ACK:播放檔案 %s 失敗", wav_filename);
                        
                        if (client_wsi) {
                            unsigned char buf[LWS_PRE + MAX_PAYLOAD];
                            unsigned char *p = &buf[LWS_PRE];
                            
                            size_t msg_len = strlen(error_msg);
                            if (msg_len > MAX_PAYLOAD - 1) {
                                msg_len = MAX_PAYLOAD - 1;
                            }
                            
                            memcpy(p, error_msg, msg_len);
                            lws_write(client_wsi, p, msg_len, LWS_WRITE_TEXT);
                        }
                    }
                } else {
                    log_with_timestamp("檔案名稱太長\n");
                }
            }
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
        "sip-audio-protocol",
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
    
    log_with_timestamp("WebSocket SIP 音頻服務器啟動\n");
    
    // 確保上傳目錄存在
    ensure_upload_directory();
    
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
    
    log_with_timestamp("WebSocket 音頻服務器監聽所有網路介面上的端口 %d\n", WS_PORT);
    log_with_timestamp("上傳目錄: %s\n", UPLOAD_DIR);
    
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
    log_with_timestamp("WebSocket 音頻服務器已關閉\n");
    
    return 0;
}