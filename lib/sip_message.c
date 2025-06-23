// sip_message.c - 實現SIP消息發送相關功能
#include "sip_client.h"

// 發送ACK請求
void send_ack(int sockfd, struct sockaddr_in *servaddr, const char *callid, const char *tag, 
              const char *branch, const char *to_tag, const char *cseq) {
    char buffer[BUF_SIZE];
    int cseq_num = atoi(cseq);
    
    log_with_timestamp("發送 ACK 給伺服器\n");
    
    snprintf(buffer, BUF_SIZE,
        "ACK sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>;tag=%s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d ACK\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "User-Agent: Custom SIP Client\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        CALLEE, SIP_SERVER,
        LOCAL_IP, LOCAL_PORT, branch,
        CALLER, USERNAME, SIP_SERVER, tag,
        CALLEE, SIP_SERVER, to_tag,
        callid,
        cseq_num,
        USERNAME, LOCAL_IP, LOCAL_PORT
    );
    
    log_with_timestamp("ACK 內容:\n%s\n", buffer);
    
    ssize_t sent_bytes = sendto(sockfd, buffer, strlen(buffer), 0, 
                           (struct sockaddr *)servaddr, sizeof(*servaddr));
                           
    if (sent_bytes < 0) {
        log_with_timestamp("錯誤: 發送 ACK 失敗: %s\n", strerror(errno));
    } else {
        log_with_timestamp("成功發送 ACK: %zd 字節\n", sent_bytes);
    }
}

// 發送BYE請求
void send_bye(int sockfd, struct sockaddr_in *servaddr, const char *callid, 
              const char *tag, const char *to_tag, const char *cseq) {
    char buffer[BUF_SIZE];
    char branch[64];
    int cseq_num = atoi(cseq);
    
    // 生成新的分支參數
    snprintf(branch, sizeof(branch), "z9hG4bK%08x", (unsigned int)time(NULL) + 3);
    
    log_with_timestamp("發送 BYE 請求給伺服器\n");
    
    snprintf(buffer, BUF_SIZE,
        "BYE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>;tag=%s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d BYE\r\n"
        "User-Agent: Custom SIP Client\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        CALLEE, SIP_SERVER,
        LOCAL_IP, LOCAL_PORT, branch,
        CALLER, USERNAME, SIP_SERVER, tag,
        CALLEE, SIP_SERVER, to_tag,
        callid,
        cseq_num + 1
    );
    
    log_with_timestamp("BYE 內容:\n%s\n", buffer);
    
    ssize_t sent_bytes = sendto(sockfd, buffer, strlen(buffer), 0, 
                           (struct sockaddr *)servaddr, sizeof(*servaddr));
                           
    if (sent_bytes < 0) {
        log_with_timestamp("錯誤: 發送 BYE 失敗: %s\n", strerror(errno));
    } else {
        log_with_timestamp("成功發送 BYE: %zd 字節\n", sent_bytes);
    }
    
    // 等待BYE的回應 (200 OK)
    char recvbuf[BUF_SIZE];
    struct sockaddr_in recv_addr;
    socklen_t recv_len = sizeof(recv_addr);
    int n;
    int timeout_count = 0;
    
    while (timeout_count < 5) {  // 最多等待5次
        memset(recvbuf, 0, BUF_SIZE);
        
        n = recv_with_timeout(sockfd, recvbuf, BUF_SIZE-1, 
                           (struct sockaddr *)&recv_addr, &recv_len, 500);
        
        if (n > 0) {
            recvbuf[n] = 0;
            char recv_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(recv_addr.sin_addr), recv_ip, INET_ADDRSTRLEN);
            
            log_with_timestamp("收到BYE回應 (%d 字節) 來自 %s:%d:\n%s\n", 
                            n, recv_ip, ntohs(recv_addr.sin_port), recvbuf);
            
            int status_code = parse_sip_status_code(recvbuf);
            if (status_code == 200) {
                log_with_timestamp("BYE請求成功，通話已結束\n");
                break;
            }
        } else if (n == 0) {
            timeout_count++;
            log_with_timestamp("等待BYE回應超時 (%d/5)\n", timeout_count);
        } else {
            log_with_timestamp("接收BYE回應錯誤: %s\n", strerror(errno));
            break;
        }
    }
}

// 初始化SIP會話
int init_sip_session(sip_session_t *session) {
    if (!session) return -1;
    
    memset(session, 0, sizeof(sip_session_t));
    
    // 創建 socket
    session->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (session->sockfd < 0) {
        log_with_timestamp("錯誤: 無法創建 socket: %s\n", strerror(errno));
        return -1;
    }
    
    log_with_timestamp("Socket 創建成功: %d\n", session->sockfd);
    
    // 增加套接字接收緩衝區大小
    int rcvbuf_size = 65536;  // 64KB
    if (setsockopt(session->sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) < 0) {
        log_with_timestamp("警告: 無法設置接收緩衝區大小: %s\n", strerror(errno));
    }
    
    // 綁定本地地址
    struct sockaddr_in localaddr;
    memset(&localaddr, 0, sizeof(localaddr));
    localaddr.sin_family = AF_INET;
    localaddr.sin_addr.s_addr = inet_addr(LOCAL_IP);
    localaddr.sin_port = htons(LOCAL_PORT);
    
    if (bind(session->sockfd, (struct sockaddr *)&localaddr, sizeof(localaddr)) < 0) {
        log_with_timestamp("錯誤: 無法綁定到本地地址 %s:%d: %s\n", 
                         LOCAL_IP, LOCAL_PORT, strerror(errno));
        close(session->sockfd);
        return -1;
    }
    
    log_with_timestamp("Socket 綁定成功: %s:%d\n", LOCAL_IP, LOCAL_PORT);
    
    // 排空可能存在的舊封包
    flush_socket(session->sockfd);
    
    // 設置伺服器地址
    memset(&session->servaddr, 0, sizeof(session->servaddr));
    session->servaddr.sin_family = AF_INET;
    session->servaddr.sin_port = htons(SIP_PORT);
    inet_pton(AF_INET, SIP_SERVER, &session->servaddr.sin_addr);
    
    // 生成SIP標識符
    get_tag(session->tag, sizeof(session->tag));
    get_callid(session->callid, sizeof(session->callid));
    snprintf(session->branch, sizeof(session->branch), "z9hG4bK%08x", (unsigned int)time(NULL));
    snprintf(session->cseq, sizeof(session->cseq), "102");
    session->remote_rtp_port = LOCAL_RTP_PORT;  // 默認RTP端口
    session->call_established = 0;
    
    log_with_timestamp("SIP 會話初始化完成:\n");
    log_with_timestamp("  - Tag: %s\n", session->tag);
    log_with_timestamp("  - Call-ID: %s\n", session->callid);
    log_with_timestamp("  - Branch: %s\n", session->branch);
    log_with_timestamp("  - CSeq: %s\n", session->cseq);
    
    return 0;
}

// 關閉SIP會話
void close_sip_session(sip_session_t *session) {
    if (!session) return;
    
    if (session->sockfd >= 0) {
        close(session->sockfd);
        session->sockfd = -1;
    }
    
    session->call_established = 0;
    log_with_timestamp("SIP 會話已關閉\n");
} 