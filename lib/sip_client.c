// sip_client.c - 實現SIP客戶端核心功能
#include "sip_client.h"

// 日誌函數
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
    
    va_end(args);
}

// MD5 摘要函數
void md5(const char *str, char *output) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5((unsigned char*)str, strlen(str), digest);
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
        sprintf(&output[i*2], "%02x", (unsigned int)digest[i]);
    output[32] = 0;
}

// 生成摘要認證響應
void make_digest_response(
    const char *username, const char *realm, const char *password,
    const char *method, const char *uri, const char *nonce,
    char *response)
{
    char ha1[33], ha2[33], kd[1024], res[33];
    char a1[256], a2[256];
    
    log_with_timestamp("生成摘要認證...\n");
    log_with_timestamp("  - 方法: %s\n", method);
    log_with_timestamp("  - URI: %s\n", uri);
    log_with_timestamp("  - 使用者名稱: %s\n", username);
    log_with_timestamp("  - 領域: %s\n", realm);
    log_with_timestamp("  - Nonce: %s\n", nonce);
    
    snprintf(a1, sizeof(a1), "%s:%s:%s", username, realm, password);
    md5(a1, ha1);
    snprintf(a2, sizeof(a2), "%s:%s", method, uri);
    md5(a2, ha2);
    snprintf(kd, sizeof(kd), "%s:%s:%s", ha1, nonce, ha2);
    md5(kd, res);
    strcpy(response, res);
    
    log_with_timestamp("  - 摘要結果: %s\n", res);
}

// 解析nonce和realm值
void parse_nonce_realm(const char *msg, char *nonce, char *realm) {
    char *p, *q;
    p = strstr(msg, "nonce=\"");
    if (p) {
        p += 7;
        q = strchr(p, '"');
        if (q) {
            strncpy(nonce, p, q-p);
            nonce[q-p] = 0;
            log_with_timestamp("解析到 nonce: %s\n", nonce);
        } else {
            log_with_timestamp("解析 nonce 失敗: 找不到結束引號\n");
        }
    } else {
        log_with_timestamp("解析 nonce 失敗: 找不到 nonce 欄位\n");
    }
    
    p = strstr(msg, "realm=\"");
    if (p) {
        p += 7;
        q = strchr(p, '"');
        if (q) {
            strncpy(realm, p, q-p);
            realm[q-p] = 0;
            log_with_timestamp("解析到 realm: %s\n", realm);
        } else {
            log_with_timestamp("解析 realm 失敗: 找不到結束引號\n");
        }
    } else {
        log_with_timestamp("解析 realm 失敗: 找不到 realm 欄位\n");
    }
}

// 生成tag
void get_tag(char *tag, size_t len) {
    snprintf(tag, len, "%08x", (unsigned int)time(NULL));
}

// 生成call-id
void get_callid(char *callid, size_t len) {
    snprintf(callid, len, "%08x@%s", (unsigned int)time(NULL), SIP_SERVER);
}

// 解析SIP消息頭
void parse_sip_headers(const char *msg) {
    char header[256];
    const char *headers[] = {"Via:", "From:", "To:", "Call-ID:", "CSeq:", "Contact:", "User-Agent:", "Content-Type:", "Content-Length:"};
    int header_count = sizeof(headers) / sizeof(headers[0]);
    
    log_with_timestamp("解析 SIP 訊息頭:\n");
    
    for (int i = 0; i < header_count; i++) {
        const char *p = strstr(msg, headers[i]);
        if (p) {
            const char *eol = strstr(p, "\r\n");
            if (eol) {
                int len = eol - p;
                if (len < sizeof(header) - 1) {
                    strncpy(header, p, len);
                    header[len] = '\0';
                    log_with_timestamp("  %s\n", header);
                }
            }
        }
    }
}

// 解析SIP響應狀態碼
int parse_sip_status_code(const char *msg) {
    int code = 0;
    if (strncmp(msg, "SIP/2.0 ", 8) == 0) {
        sscanf(msg + 8, "%d", &code);
    }
    return code;
}

// 提取To標籤
char* extract_to_tag(const char *msg, char *tag_buf, size_t buf_size) {
    char *to_header = strstr(msg, "To:");
    if (to_header) {
        char *tag_start = strstr(to_header, "tag=");
        if (tag_start) {
            tag_start += 4;  // 跳過 "tag="
            char *tag_end = strpbrk(tag_start, "\r\n;>");
            if (tag_end) {
                size_t tag_len = tag_end - tag_start;
                if (tag_len < buf_size - 1) {
                    strncpy(tag_buf, tag_start, tag_len);
                    tag_buf[tag_len] = '\0';
                    return tag_buf;
                }
            }
        }
    }
    return NULL;
}

// 帶超時的接收函數
int recv_with_timeout(int sockfd, char *buf, int maxlen, struct sockaddr *src_addr, socklen_t *addrlen, int timeout_ms) {
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    int ret = select(sockfd + 1, &readfds, NULL, NULL, &tv);
    if (ret < 0) {
        log_with_timestamp("select() 錯誤: %s\n", strerror(errno));
        return -1;  // 錯誤
    } else if (ret == 0) {
        return 0;  // 超時，無數據可讀
    }
    
    // 有數據可讀
    return recvfrom(sockfd, buf, maxlen, 0, src_addr, addrlen);
}

// 清空socket接收緩衝區
void flush_socket(int sockfd) {
    char temp_buf[BUF_SIZE];
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int flags = fcntl(sockfd, F_GETFL, 0);
    
    // 設置非阻塞模式
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    
    // 排空緩衝區
    while (recvfrom(sockfd, temp_buf, BUF_SIZE, 0, (struct sockaddr*)&addr, &addr_len) > 0) {
        log_with_timestamp("排出舊封包\n");
    }
    
    // 恢復原來的 flags
    fcntl(sockfd, F_SETFL, flags);
}

// 函數：解析SIP訊息中的RTP端口
int parse_rtp_port(const char *msg) {
    char *sdp_start = strstr(msg, "\r\n\r\n");
    if (!sdp_start) return 0;
    
    sdp_start += 4; // 跳過分隔符
    
    // 尋找媒體描述行 (m=audio)
    char *m_audio = strstr(sdp_start, "m=audio ");
    if (!m_audio) return 0;
    
    // 解析端口號
    int port = 0;
    if (sscanf(m_audio, "m=audio %d", &port) != 1) {
        return 0;
    }
    
    return port;
} 