#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

// WebSocket 客戶端配置
#define DEFAULT_SERVER_ADDRESS "0.0.0.0"
#define DEFAULT_SERVER_PORT 8080
#define MAX_PAYLOAD (200 * 1024)  // 200KB，足夠處理大部分 WAV 檔案
#define MAX_FILE_SIZE (1024 * 1024)  // 最大 1MB WAV 檔案
#define WAV_HEADER_SIZE 64

// 全局變量
static struct lws_context *context;
static struct lws *wsi_client = NULL;
static volatile int force_exit = 0;
static volatile int connected = 0;
static int rtp_packet_count = 0;
static char server_address[128] = DEFAULT_SERVER_ADDRESS;
static int server_port = DEFAULT_SERVER_PORT;

// 緩存接收到的 RTP 數據
typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} rtp_buffer_t;

static rtp_buffer_t g_rtp_buffer = {NULL, 0, 0};

// WAV 檔案上傳狀態
typedef struct {
    char filename[256];
    unsigned char *data;
    size_t size;
    size_t sent;
    int in_progress;
} wav_upload_t;

static wav_upload_t g_wav_upload = {0};

// Base64 編碼表
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 簡單的日誌函數
void log_with_timestamp(const char *format, ...) {
    time_t now;
    struct tm *timeinfo;
    char timestamp[30];
    
    time(&now);
    timeinfo = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]", timeinfo);
    
    va_list args;
    va_start(args, format);
    
    printf("%s ", timestamp);
    vprintf(format, args);
    fflush(stdout);
    
    va_end(args);
}

// Base64 編碼函數
char* base64_encode(const unsigned char* data, size_t input_length) {
    size_t output_length = 4 * ((input_length + 2) / 3);
    char *encoded_data = malloc(output_length + 1);
    
    if (encoded_data == NULL) return NULL;
    
    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;
        
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
        
        encoded_data[j++] = base64_chars[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 0 * 6) & 0x3F];
    }
    
    // 添加 padding
    int mod_table[] = {0, 2, 1};
    for (int i = 0; i < mod_table[input_length % 3]; i++) {
        encoded_data[output_length - 1 - i] = '=';
    }
    
    encoded_data[output_length] = '\0';
    return encoded_data;
}

// 初始化 RTP 緩衝區
void init_rtp_buffer() {
    g_rtp_buffer.capacity = 1024 * 1024;  // 初始分配 1MB
    g_rtp_buffer.data = (unsigned char*)malloc(g_rtp_buffer.capacity);
    g_rtp_buffer.size = 0;
    
    if (!g_rtp_buffer.data) {
        log_with_timestamp("錯誤: 無法分配 RTP 緩衝區記憶體\n");
        g_rtp_buffer.capacity = 0;
    }
}

// 釋放 RTP 緩衝區
void free_rtp_buffer() {
    if (g_rtp_buffer.data) {
        free(g_rtp_buffer.data);
        g_rtp_buffer.data = NULL;
    }
    g_rtp_buffer.size = 0;
    g_rtp_buffer.capacity = 0;
}

// 添加 RTP 數據到緩衝區
void add_rtp_data(const unsigned char *data, size_t len) {
    // 檢查是否需要擴展緩衝區
    if (g_rtp_buffer.size + len > g_rtp_buffer.capacity) {
        size_t new_capacity = g_rtp_buffer.capacity * 2;
        unsigned char *new_data = (unsigned char*)realloc(g_rtp_buffer.data, new_capacity);
        
        if (!new_data) {
            log_with_timestamp("錯誤: 無法擴展 RTP 緩衝區\n");
            return;
        }
        
        g_rtp_buffer.data = new_data;
        g_rtp_buffer.capacity = new_capacity;
        log_with_timestamp("RTP 緩衝區擴展到 %zu 字節\n", new_capacity);
    }
    
    // 添加數據到緩衝區
    memcpy(g_rtp_buffer.data + g_rtp_buffer.size, data, len);
    g_rtp_buffer.size += len;
}

// 生成 WAV 文件頭 (PCMU/G.711 μ-law 格式, 8000Hz, 單聲道)
void generate_wav_header(unsigned char *header, size_t data_size) {
    // RIFF 頭部
    memcpy(header, "RIFF", 4);
    unsigned int filesize = data_size + WAV_HEADER_SIZE - 8;
    header[4] = (filesize) & 0xff;
    header[5] = (filesize >> 8) & 0xff;
    header[6] = (filesize >> 16) & 0xff;
    header[7] = (filesize >> 24) & 0xff;
    
    // WAVE 標識
    memcpy(header + 8, "WAVE", 4);
    
    // fmt 區塊
    memcpy(header + 12, "fmt ", 4);
    // fmt 區塊大小 (18 字節)
    header[16] = 18;
    header[17] = 0;
    header[18] = 0;
    header[19] = 0;
    
    // 音頻格式: 7 = MULAW
    header[20] = 7;
    header[21] = 0;
    
    // 通道數: 1 = 單聲道
    header[22] = 1;
    header[23] = 0;
    
    // 採樣率: 8000Hz
    header[24] = 0x40;
    header[25] = 0x1F;
    header[26] = 0;
    header[27] = 0;
    
    // 字節率: 8000 字節/秒
    header[28] = 0x40;
    header[29] = 0x1F;
    header[30] = 0;
    header[31] = 0;
    
    // 數據塊對齊: 1 字節
    header[32] = 1;
    header[33] = 0;
    
    // 位寬: 8 位
    header[34] = 8;
    header[35] = 0;
    
    // 額外參數大小: 0
    header[36] = 0;
    header[37] = 0;
    
    // fact 區塊 (可選)
    memcpy(header + 38, "fact", 4);
    // fact 區塊大小 (4 字節)
    header[42] = 4;
    header[43] = 0;
    header[44] = 0;
    header[45] = 0;
    
    // 樣本數
    unsigned int samples = data_size;
    header[46] = (samples) & 0xff;
    header[47] = (samples >> 8) & 0xff;
    header[48] = (samples >> 16) & 0xff;
    header[49] = (samples >> 24) & 0xff;
    
    // data 區塊
    memcpy(header + 50, "data", 4);
    // data 區塊大小
    header[54] = (data_size) & 0xff;
    header[55] = (data_size >> 8) & 0xff;
    header[56] = (data_size >> 16) & 0xff;
    header[57] = (data_size >> 24) & 0xff;
    
    // 剩餘部分填充 0
    for (int i = 58; i < WAV_HEADER_SIZE; i++) {
        header[i] = 0;
    }
}

// 將緩存的 RTP 數據保存為 WAV 文件
int save_rtp_to_wav(const char *filename) {
    if (!g_rtp_buffer.data || g_rtp_buffer.size == 0) {
        log_with_timestamp("錯誤: 沒有 RTP 數據可保存\n");
        return -1;
    }
    
    // 打開輸出文件
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_with_timestamp("錯誤: 無法創建輸出文件 %s: %s\n", filename, strerror(errno));
        return -1;
    }
    
    // 生成 WAV 頭部
    unsigned char header[WAV_HEADER_SIZE];
    generate_wav_header(header, g_rtp_buffer.size);
    
    // 寫入 WAV 頭部
    if (write(fd, header, WAV_HEADER_SIZE) != WAV_HEADER_SIZE) {
        log_with_timestamp("錯誤: 寫入 WAV 頭部失敗: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    
    // 寫入 RTP 數據
    ssize_t written = write(fd, g_rtp_buffer.data, g_rtp_buffer.size);
    if (written < 0 || (size_t)written != g_rtp_buffer.size) {
        log_with_timestamp("錯誤: 寫入 RTP 數據失敗: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    
    close(fd);
    log_with_timestamp("成功保存 %zu 字節的 RTP 數據到 %s\n", g_rtp_buffer.size, filename);
    return 0;
}

// 讀取 WAV 檔案並準備上傳
int prepare_wav_upload(const char *filename) {
    // 清理之前的上傳數據
    if (g_wav_upload.data) {
        free(g_wav_upload.data);
        g_wav_upload.data = NULL;
    }
    
    // 檢查檔案是否存在
    struct stat st;
    if (stat(filename, &st) != 0) {
        log_with_timestamp("錯誤: 無法存取檔案 %s: %s\n", filename, strerror(errno));
        return -1;
    }
    
    if (st.st_size > MAX_FILE_SIZE) {
        log_with_timestamp("錯誤: 檔案 %s 太大 (%ld 字節)，最大允許 %d 字節\n", 
                          filename, st.st_size, MAX_FILE_SIZE);
        return -1;
    }
    
    // 讀取檔案
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        log_with_timestamp("錯誤: 無法開啟檔案 %s: %s\n", filename, strerror(errno));
        return -1;
    }
    
    g_wav_upload.data = malloc(st.st_size);
    if (!g_wav_upload.data) {
        log_with_timestamp("錯誤: 無法分配記憶體\n");
        fclose(fp);
        return -1;
    }
    
    size_t read_size = fread(g_wav_upload.data, 1, st.st_size, fp);
    fclose(fp);
    
    if (read_size != st.st_size) {
        log_with_timestamp("錯誤: 讀取檔案失敗\n");
        free(g_wav_upload.data);
        g_wav_upload.data = NULL;
        return -1;
    }
    
    // 設置上傳資訊
    strncpy(g_wav_upload.filename, filename, sizeof(g_wav_upload.filename) - 1);
    g_wav_upload.filename[sizeof(g_wav_upload.filename) - 1] = '\0';
    g_wav_upload.size = st.st_size;
    g_wav_upload.sent = 0;
    g_wav_upload.in_progress = 0;
    
    log_with_timestamp("準備上傳 WAV 檔案: %s (%zu 字節)\n", filename, g_wav_upload.size);
    return 0;
}

// 從十六進制字符串轉換為二進制數據
size_t hex_to_bin(const char *hex_str, size_t hex_len, unsigned char *bin_data) {
    size_t bin_len = 0;
    
    for (size_t i = 0; i < hex_len; i += 2) {
        unsigned int value;
        if (sscanf(hex_str + i, "%2x", &value) == 1) {
            bin_data[bin_len++] = (unsigned char)value;
        }
    }
    
    return bin_len;
}

// WebSocket 客戶端回調函數
static int callback_client(struct lws *wsi, enum lws_callback_reasons reason,
                          void *user, void *in, size_t len) {
    
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            log_with_timestamp("WebSocket 客戶端連接建立\n");
            connected = 1;
            break;
            
        case LWS_CALLBACK_CLIENT_RECEIVE:
            {
                char *msg = (char*)in;
                
                // 檢查是否為 RTP 數據
                if (strncmp(msg, "RTP:", 4) == 0) {
                    rtp_packet_count++;
                    
                    // 解析十六進制數據長度
                    size_t hex_data_len = len - 4;
                    size_t rtp_data_len = hex_data_len / 2;
                    
                                         // 只在前幾個封包或每100個封包顯示詳細資訊
                    if (rtp_packet_count <= 5 || rtp_packet_count % 100 == 0) {
                        log_with_timestamp("收到 RTP 封包 #%d，原始大小: %zu 字節\n", 
                                         rtp_packet_count, rtp_data_len);
                    }
                    
                    // 只在前3個封包顯示十六進制數據
                    if (hex_data_len > 0 && rtp_packet_count <= 3) {
                        char hex_sample[33] = {0};
                        size_t sample_len = (hex_data_len > 32) ? 32 : hex_data_len;
                        strncpy(hex_sample, msg + 4, sample_len);
                        log_with_timestamp("RTP 數據樣本 (前16字節): %s%s\n", 
                                         hex_sample, (hex_data_len > 32) ? "..." : "");
                        
                        // 將十六進制數據轉換為二進制並保存到緩衝區
                        if (g_rtp_buffer.data) {
                            // 分配臨時緩衝區
                            unsigned char *bin_data = (unsigned char*)malloc(rtp_data_len);
                            if (bin_data) {
                                // 轉換並提取 RTP 有效載荷 (跳過 RTP 頭部 12 字節)
                                size_t bin_len = hex_to_bin(msg + 4, hex_data_len, bin_data);
                                if (bin_len > 12) {  // 確保有效載荷存在
                                    // 添加有效載荷到緩衝區 (跳過 RTP 頭部)
                                    add_rtp_data(bin_data + 12, bin_len - 12);
                                }
                                free(bin_data);
                            }
                        }
                    }
                    
                    // 每 100 個封包顯示一次統計
                    if (rtp_packet_count % 100 == 0) {
                        log_with_timestamp("已接收 %d 個 RTP 封包，已緩存 %zu 字節音頻數據\n", 
                                          rtp_packet_count, g_rtp_buffer.size);
                    }
                } else if (strncmp(msg, "WAV_ACK:", 8) == 0) {
                    log_with_timestamp("收到 WAV 上傳確認: %.*s\n", (int)(len - 8), msg + 8);
                } else {
                    log_with_timestamp("收到服務器消息: %.*s\n", (int)len, (char*)in);
                }
            }
            break;
            
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            log_with_timestamp("WebSocket 連接錯誤\n");
            connected = 0;
            force_exit = 1;
            break;
            
        case LWS_CALLBACK_CLOSED:
            log_with_timestamp("WebSocket 連接關閉\n");
            connected = 0;
            force_exit = 1;
            break;
            
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            // 可以發送數據
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
        callback_client,
        0,
        MAX_PAYLOAD,
    },
    { NULL, NULL, 0, 0 } // 結束標記
};

// 信號處理函數
void sigint_handler(int sig) {
    log_with_timestamp("收到中斷信號，正在關閉客戶端...\n");
    force_exit = 1;
    if (context) {
        lws_cancel_service(context);
    }
}

// 發送消息到服務器
void send_message_to_server(const char *message) {
    if (wsi_client && connected) {
        unsigned char buf[LWS_PRE + MAX_PAYLOAD];
        unsigned char *p = &buf[LWS_PRE];
        
        size_t msg_len = strlen(message);
        if (msg_len > MAX_PAYLOAD - 1) {
            msg_len = MAX_PAYLOAD - 1;
        }
        
        memcpy(p, message, msg_len);
        
        if (lws_write(wsi_client, p, msg_len, LWS_WRITE_TEXT) < 0) {
            log_with_timestamp("發送消息失敗\n");
        } else {
            log_with_timestamp("發送消息: %s\n", message);
        }
    } else {
        log_with_timestamp("WebSocket 未連接，無法發送消息\n");
    }
}

// 上傳 WAV 檔案到服務器
int upload_wav_file(const char *filename) {
    if (!wsi_client || !connected) {
        log_with_timestamp("WebSocket 未連接，無法上傳檔案\n");
        return -1;
    }
    
    if (prepare_wav_upload(filename) != 0) {
        return -1;
    }
    
    // Base64 編碼 WAV 數據
    char *encoded_data = base64_encode(g_wav_upload.data, g_wav_upload.size);
    if (!encoded_data) {
        log_with_timestamp("錯誤: Base64 編碼失敗\n");
        return -1;
    }
    
    // 準備上傳消息
    size_t msg_len = strlen("WAV_UPLOAD:") + strlen(filename) + 1 + strlen(encoded_data) + 1;
    char *upload_msg = malloc(msg_len);
    if (!upload_msg) {
        log_with_timestamp("錯誤: 無法分配上傳消息記憶體\n");
        free(encoded_data);
        return -1;
    }
    
    snprintf(upload_msg, msg_len, "WAV_UPLOAD:%s:%s", filename, encoded_data);
    
    log_with_timestamp("開始上傳 WAV 檔案 %s (編碼後大小: %zu 字節)\n", 
                      filename, strlen(encoded_data));
    
    // 發送上傳消息
    send_message_to_server(upload_msg);
    
    // 清理
    free(upload_msg);
    free(encoded_data);
    
    return 0;
}

// 用戶界面函數
void show_menu() {
    printf("\n=== WebSocket SIP 音頻 Demo 客戶端 ===\n");
    printf("1. 撥打電話 (預設號碼: 0938220136)\n");
    printf("2. 撥打電話 (自定義號碼)\n");
    printf("3. 掛斷電話\n");
    printf("4. 顯示統計\n");
    printf("5. 上傳 WAV 檔案\n");
    printf("6. 播放指定 WAV 檔案\n");
    printf("7. 將接收的 RTP 封包保存為 WAV 文件\n");
    printf("8. 退出\n");
    printf("請選擇 (1-8): ");
    fflush(stdout);
}

// 處理用戶輸入
void handle_user_input() {
    char input[256];
    char phone_number[64];
    char filename[256];
    
    while (!force_exit && connected) {
        show_menu();
        
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        
        int choice = atoi(input);
        
        switch (choice) {
            case 1:
                send_message_to_server("CALL:0938220136");
                log_with_timestamp("發起撥打預設號碼的請求\n");
                break;
                
            case 2:
                printf("請輸入電話號碼: ");
                fflush(stdout);
                if (fgets(phone_number, sizeof(phone_number), stdin)) {
                    // 移除換行符
                    phone_number[strcspn(phone_number, "\n")] = 0;
                    
                    // 檢查電話號碼格式是否正確
                    int valid = 1;
                    for (int i = 0; phone_number[i]; i++) {
                        if (phone_number[i] < '0' || phone_number[i] > '9') {
                            valid = 0;
                            break;
                        }
                    }
                    
                    if (!valid || strlen(phone_number) < 3) {
                        log_with_timestamp("無效的電話號碼格式，請使用純數字\n");
                        break;
                    }
                    
                    char call_msg[80];
                    snprintf(call_msg, sizeof(call_msg), "CALL:%s", phone_number);
                    send_message_to_server(call_msg);
                    log_with_timestamp("發起撥打 %s 的請求\n", phone_number);
                }
                break;
                
            case 3:
                send_message_to_server("HANGUP");
                log_with_timestamp("發送掛斷請求\n");
                break;
                
            case 4:
                log_with_timestamp("統計信息：已接收 %d 個 RTP 封包，已緩存 %zu 字節音頻數據\n", 
                                  rtp_packet_count, g_rtp_buffer.size);
                break;
                
            case 5:
                printf("請輸入要上傳的 WAV 檔案路徑: ");
                fflush(stdout);
                if (fgets(filename, sizeof(filename), stdin)) {
                    // 移除換行符
                    filename[strcspn(filename, "\n")] = 0;
                    
                    if (strlen(filename) > 0) {
                        upload_wav_file(filename);
                    } else {
                        log_with_timestamp("無效的檔案路徑\n");
                    }
                }
                break;
                
            case 6:
                printf("請輸入要播放的 WAV 檔案名稱: ");
                fflush(stdout);
                if (fgets(filename, sizeof(filename), stdin)) {
                    // 移除換行符
                    filename[strcspn(filename, "\n")] = 0;
                    
                    if (strlen(filename) > 0) {
                        char play_msg[300];
                        snprintf(play_msg, sizeof(play_msg), "PLAY_WAV:%s", filename);
                        send_message_to_server(play_msg);
                        log_with_timestamp("發送播放 WAV 檔案請求: %s\n", filename);
                    } else {
                        log_with_timestamp("無效的檔案名稱\n");
                    }
                }
                break;
                
            case 7:
                {
                    char wav_filename[256];
                    time_t now = time(NULL);
                    struct tm *timeinfo = localtime(&now);
                    
                    // 生成帶時間戳的文件名
                    strftime(wav_filename, sizeof(wav_filename), "rtp_capture_%Y%m%d_%H%M%S.wav", timeinfo);
                    
                    log_with_timestamp("正在將接收到的 RTP 數據保存為 WAV 文件: %s\n", wav_filename);
                    if (save_rtp_to_wav(wav_filename) == 0) {
                        log_with_timestamp("WAV 文件保存成功！\n");
                    } else {
                        log_with_timestamp("保存 WAV 文件失敗\n");
                    }
                }
                break;
                
            case 8:
                log_with_timestamp("用戶選擇退出\n");
                force_exit = 1;
                return;
                
            default:
                printf("無效選擇，請重新輸入\n");
                break;
        }
        
        // 給 WebSocket 處理一些時間
        usleep(100000); // 100ms
    }
}

int main(int argc, char **argv) {
    struct lws_context_creation_info info;
    struct lws_client_connect_info connect_info;
    
    // 解析命令行參數
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0 || strcmp(argv[i], "-s") == 0) {
            if (i + 1 < argc) {
                strncpy(server_address, argv[i + 1], sizeof(server_address) - 1);
                server_address[sizeof(server_address) - 1] = '\0';
                i++;
            }
        } else if (strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) {
            if (i + 1 < argc) {
                server_port = atoi(argv[i + 1]);
                i++;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("使用方式：%s [選項]\n", argv[0]);
            printf("選項：\n");
            printf("  --server, -s <地址>    設置服務器地址 (默認: %s)\n", DEFAULT_SERVER_ADDRESS);
            printf("  --port, -p <端口>      設置服務器端口 (默認: %d)\n", DEFAULT_SERVER_PORT);
            printf("  --help, -h             顯示此幫助信息\n");
            return 0;
        }
    }
    
    log_with_timestamp("WebSocket SIP 音頻 Demo 客戶端啟動\n");
    log_with_timestamp("服務器地址: %s:%d\n", server_address, server_port);
    
    // 初始化 RTP 緩衝區
    init_rtp_buffer();
    
    // 設置信號處理
    signal(SIGINT, sigint_handler);
    
    // 初始化 libwebsockets
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    
    context = lws_create_context(&info);
    if (!context) {
        log_with_timestamp("創建 WebSocket 上下文失敗\n");
        free_rtp_buffer();
        return -1;
    }
    
    // 連接到服務器
    memset(&connect_info, 0, sizeof(connect_info));
    connect_info.context = context;
    connect_info.address = server_address;
    connect_info.port = server_port;
    connect_info.path = "/";
    connect_info.host = server_address;
    connect_info.origin = server_address;
    connect_info.protocol = "sip-audio-protocol";
    
    log_with_timestamp("正在連接到 %s:%d\n", server_address, server_port);
    
    wsi_client = lws_client_connect_via_info(&connect_info);
    if (!wsi_client) {
        log_with_timestamp("創建客戶端連接失敗\n");
        lws_context_destroy(context);
        free_rtp_buffer();
        return -1;
    }
    
    // 等待連接建立
    int connection_timeout = 50; // 5 秒超時
    while (!connected && !force_exit && connection_timeout > 0) {
        lws_service(context, 100);
        connection_timeout--;
    }
    
    if (!connected) {
        log_with_timestamp("連接到服務器超時\n");
        lws_context_destroy(context);
        free_rtp_buffer();
        return -1;
    }
    
    log_with_timestamp("成功連接到服務器\n");
    
    // 創建處理用戶輸入的線程
    pthread_t input_thread;
    if (pthread_create(&input_thread, NULL, (void*)handle_user_input, NULL) != 0) {
        log_with_timestamp("創建輸入線程失敗\n");
        force_exit = 1;
    }
    
    // 主循環
    while (!force_exit) {
        lws_service(context, 50);
    }
    
    // 清理
    if (!force_exit) {
        pthread_join(input_thread, NULL);
    }
    
    // 清理 WAV 上傳數據
    if (g_wav_upload.data) {
        free(g_wav_upload.data);
    }
    
    lws_context_destroy(context);
    free_rtp_buffer();
    log_with_timestamp("WebSocket 音頻客戶端已關閉\n");
    log_with_timestamp("總計接收了 %d 個 RTP 封包\n", rtp_packet_count);
    
    return 0;
} 