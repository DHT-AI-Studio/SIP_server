// sip_call.c - 實現SIP呼叫控制功能
#include "sip_client.h"

// 發起SIP呼叫
int make_sip_call(sip_session_t *session, const char *callee) {
    if (!session || session->sockfd < 0) return -1;
    
    char buffer[BUF_SIZE], recvbuf[BUF_SIZE];
    char nonce[256] = "", realm[256] = "";
    char response[33];
    char auth_header[1024] = "";
    char sdp[BUF_SIZE];
    int received_100 = 0, received_183 = 0, received_200 = 0;
    struct sockaddr_in recv_addr;
    
    log_with_timestamp("準備發起SIP呼叫到 %s\n", callee);
    
    // 構建SDP內容 - 使用動態RTP接收端口，與網關端口範圍匹配
    // 網關通常使用32000-32011範圍，我們也應該在此範圍內協商
    int suggested_rtp_port = LOCAL_RTP_PORT;  // 起始建議端口
    snprintf(sdp, BUF_SIZE,
        "v=0\r\n"
        "o=- 0 0 IN IP4 " LOCAL_IP "\r\n"
        "s=Custom SIP Client\r\n"
        "c=IN IP4 " LOCAL_IP "\r\n"
        "t=0 0\r\n"
        "m=audio %d RTP/AVP 0 8 101\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=rtpmap:101 telephone-event/8000\r\n"
        "a=fmtp:101 0-16\r\n"
        "a=ptime:20\r\n"
        "a=sendrecv\r\n",
        suggested_rtp_port  // 建議端口，最終以對方回應為準
    );
    log_with_timestamp("SDP中建議的RTP端口: %d（最終端口以對方回應為準）\n", suggested_rtp_port);
    int sdp_len = strlen(sdp);
    
    // 構建初始 INVITE 請求
    snprintf(buffer, BUF_SIZE,
        "INVITE sip:%s@%s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
        "Max-Forwards: 70\r\n"
        "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %s INVITE\r\n"
        "User-Agent: Custom SIP Client\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        callee, SIP_SERVER,
        LOCAL_IP, LOCAL_PORT, session->branch,
        CALLER, CALLER, SIP_SERVER, session->tag,
        callee, SIP_SERVER,
        CALLER, LOCAL_IP, LOCAL_PORT,
        session->callid,
        session->cseq,
        sdp_len,
        sdp
    );
    
    log_with_timestamp("發送初始 INVITE 請求 (%lu 字節):\n%s\n", strlen(buffer), buffer);
    
    // 清空接收緩衝區
    flush_socket(session->sockfd);
    
    ssize_t sent_bytes = sendto(session->sockfd, buffer, strlen(buffer), 0, 
                            (struct sockaddr *)&session->servaddr, sizeof(session->servaddr));
    
    if (sent_bytes < 0) {
        log_with_timestamp("錯誤: 發送 INVITE 失敗: %s\n", strerror(errno));
        return -1;
    }
    
    log_with_timestamp("成功發送 %zd 字節\n", sent_bytes);
    log_with_timestamp("等待 SIP 回應...\n");
    
    // 用於接收的循環
    int timeout_count = 0;
    
    // 直接嘗試接收回應
    while (timeout_count < 30) {  // 增加到30次嘗試
        memset(recvbuf, 0, BUF_SIZE);
        socklen_t recv_len = sizeof(recv_addr);
        
        // 使用較短的超時(500ms)多次檢查
        int n = recv_with_timeout(session->sockfd, recvbuf, BUF_SIZE-1, 
                               (struct sockaddr *)&recv_addr, &recv_len, 500);
        
        if (n > 0) {
            // 成功接收
            recvbuf[n] = 0;
            char recv_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(recv_addr.sin_addr), recv_ip, INET_ADDRSTRLEN);
            
            log_with_timestamp("收到資料 (%d 字節) 來自 %s:%d:\n%s\n", 
                            n, recv_ip, ntohs(recv_addr.sin_port), recvbuf);
            
            // 解析回應狀態碼
            int status_code = parse_sip_status_code(recvbuf);
            log_with_timestamp("SIP 回應狀態碼: %d\n", status_code);
            
            // 解析 SIP 訊息頭
            parse_sip_headers(recvbuf);
            
            // 根據狀態碼處理回應
            if (status_code == 100) {
                log_with_timestamp("收到 100 Trying\n");
                received_100 = 1;
            } else if (status_code == 183) {
                log_with_timestamp("收到 183 Session Progress\n");
                received_183 = 1;
                
                // 提取 To tag 以備後用
                if (extract_to_tag(recvbuf, session->to_tag, sizeof(session->to_tag))) {
                    log_with_timestamp("提取到 To tag: %s\n", session->to_tag);
                }
            } else if (status_code == 200) {
                log_with_timestamp("收到 200 OK\n");
                received_200 = 1;
                
                // 提取 To tag (如果尚未提取)
                if (session->to_tag[0] == '\0' && extract_to_tag(recvbuf, session->to_tag, sizeof(session->to_tag))) {
                    log_with_timestamp("提取到 To tag: %s\n", session->to_tag);
                }
                
                // 解析 SDP 訊息
                const char *sdp_start = strstr(recvbuf, "\r\n\r\n");
                if (sdp_start) {
                    sdp_start += 4;  // 跳過 \r\n\r\n
                    log_with_timestamp("SDP 內容:\n%s\n", sdp_start);
                    
                    // 解析媒體端口
                    const char *m_line = strstr(sdp_start, "m=audio ");
                    if (m_line) {
                        sscanf(m_line, "m=audio %d", &session->remote_rtp_port);
                        log_with_timestamp("解析到 RTP 端口: %d\n", session->remote_rtp_port);
                    } else {
                        log_with_timestamp("找不到音頻媒體行\n");
                    }
                }
                
                // 發送 ACK
                char new_branch[64];
                snprintf(new_branch, sizeof(new_branch), "z9hG4bK%08x", (unsigned int)time(NULL) + 2);
                send_ack(session->sockfd, &session->servaddr, session->callid, session->tag, 
                        new_branch, session->to_tag, session->cseq);
                
                session->call_established = 1;
                
                // 收到 200 OK 並發送 ACK 後，可以退出接收循環
                break;
                
            } else if (status_code == 401 || status_code == 407) {
                log_with_timestamp("收到認證請求: %d\n", status_code);
                parse_nonce_realm(recvbuf, nonce, realm);
                
                if (strlen(nonce) == 0 || strlen(realm) == 0) {
                    log_with_timestamp("認證資訊解析失敗\n");
                    return -1;
                }
                
                char uri[128];
                snprintf(uri, sizeof(uri), "sip:%s@%s", callee, SIP_SERVER);
                log_with_timestamp("準備帶認證的 INVITE 請求\n");
                
                make_digest_response(USERNAME, realm, PASSWORD, "INVITE", uri, nonce, response);
                
                snprintf(auth_header, sizeof(auth_header),
                    "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\"\r\n",
                    USERNAME, realm, nonce, uri, response);
                
                log_with_timestamp("認證標頭: %s", auth_header);
                
                // 生成新的分支參數
                snprintf(session->branch, sizeof(session->branch), "z9hG4bK%08x", (unsigned int)time(NULL)+1);
                log_with_timestamp("新分支參數: %s\n", session->branch);
                
                // 構建帶認證的 INVITE 請求
                snprintf(buffer, BUF_SIZE,
                    "INVITE sip:%s@%s SIP/2.0\r\n"
                    "Via: SIP/2.0/UDP %s:%d;branch=%s\r\n"
                    "Max-Forwards: 70\r\n"
                    "From: <sip:%s@%s>;tag=%s\r\n"
                    "To: <sip:%s@%s>\r\n"
                    "Contact: <sip:%s@%s:%d>\r\n"
                    "Call-ID: %s\r\n"
                    "CSeq: %s INVITE\r\n"
                    "User-Agent: Custom SIP Client\r\n"
                    "%s"
                    "Content-Type: application/sdp\r\n"
                    "Content-Length: %d\r\n"
                    "\r\n"
                    "%s",
                    callee, SIP_SERVER,
                    LOCAL_IP, LOCAL_PORT, session->branch,
                    CALLER, SIP_SERVER, session->tag,
                    callee, SIP_SERVER,
                    CALLER, LOCAL_IP, LOCAL_PORT,
                    session->callid,
                    session->cseq,
                    auth_header,
                    sdp_len,
                    sdp
                );
                
                log_with_timestamp("發送帶認證的 INVITE 請求 (%lu 字節):\n%s\n", strlen(buffer), buffer);
                
                // 清空接收緩衝區
                flush_socket(session->sockfd);
                
                sent_bytes = sendto(session->sockfd, buffer, strlen(buffer), 0, 
                                 (struct sockaddr *)&session->servaddr, sizeof(session->servaddr));
                
                if (sent_bytes < 0) {
                    log_with_timestamp("錯誤: 發送認證 INVITE 失敗\n");
                    return -1;
                }
                
                log_with_timestamp("成功發送認證 INVITE: %zd 字節\n", sent_bytes);
                
            } else if (status_code == 403) {
                log_with_timestamp("權限被拒絕: 403 Forbidden\n");
                return -1;
            } else {
                log_with_timestamp("收到其他狀態碼: %d\n", status_code);
            }
            
            // 重置超時計數
            timeout_count = 0;
            
        } else if (n == 0) {
            // 超時，無數據可讀
            timeout_count++;
            log_with_timestamp("接收超時 (%d/30)\n", timeout_count);
        } else {
            // 接收錯誤
            log_with_timestamp("接收錯誤: %s\n", strerror(errno));
            break;
        }
    }
    
    // 總結會話狀態
    log_with_timestamp("SIP 呼叫結果:\n");
    log_with_timestamp("  - 接收到 100 Trying: %s\n", received_100 ? "是" : "否");
    log_with_timestamp("  - 接收到 183 Session Progress: %s\n", received_183 ? "是" : "否");
    log_with_timestamp("  - 接收到 200 OK: %s\n", received_200 ? "是" : "否");
    log_with_timestamp("  - 通話建立: %s\n", session->call_established ? "是" : "否");
    
    return session->call_established ? 0 : -1;
} 