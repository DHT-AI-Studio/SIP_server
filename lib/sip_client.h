// sip_client.h
#ifndef SIP_CLIENT_H
#define SIP_CLIENT_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <openssl/md5.h>
#include <time.h>
#include <sys/select.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>

// 常量定義
#define SIP_SERVER "192.168.1.170"
#define SIP_PORT 5060
#define LOCAL_IP "192.168.157.126"
#define LOCAL_PORT 5062
#define LOCAL_RTP_PORT 32000
#define LOCAL_RTP_SEND_PORT 32001  // 發送RTP用的端口，保持在網關範圍內
#define BUF_SIZE 4096

// RTP和音頻相關常數
#define RTP_PACKET_SIZE 160  // G.711 ulaw 20ms@8kHz = 160 bytes
#define WAV_HEADER_SIZE 64   // μ-law WAV 頭部大小

#define USERNAME "voip"
#define PASSWORD "qwER12#$"
#define CALLER "0921367101"
#define CALLEE "0938220136"

// RTP包頭結構
typedef struct {
    unsigned char version_p_x_cc;
    unsigned char m_pt;
    unsigned short seq_num;
    unsigned int timestamp;
    unsigned int ssrc;
} rtp_header_t;

// SIP會話狀態
typedef struct {
    int sockfd;
    char tag[32];
    char callid[64];
    char branch[64];
    char cseq[16];
    char to_tag[128];
    int remote_rtp_port;
    struct sockaddr_in servaddr;
    int call_established;
} sip_session_t;

// 日誌函數
void log_with_timestamp(const char *format, ...);

// SIP協議相關函數
void md5(const char *str, char *output);
void make_digest_response(const char *username, const char *realm, const char *password,
                         const char *method, const char *uri, const char *nonce,
                         char *response);
void parse_nonce_realm(const char *msg, char *nonce, char *realm);
void get_tag(char *tag, size_t len);
void get_callid(char *callid, size_t len);
void parse_sip_headers(const char *msg);
int parse_sip_status_code(const char *msg);
char* extract_to_tag(const char *msg, char *tag_buf, size_t buf_size);
int recv_with_timeout(int sockfd, char *buf, int maxlen, struct sockaddr *src_addr, socklen_t *addrlen, int timeout_ms);
void flush_socket(int sockfd);
int parse_rtp_port(const char *msg);

// SIP消息發送函數
void send_ack(int sockfd, struct sockaddr_in *servaddr, const char *callid, const char *tag, 
             const char *branch, const char *to_tag, const char *cseq);
void send_bye(int sockfd, struct sockaddr_in *servaddr, const char *callid, 
             const char *tag, const char *to_tag, const char *cseq);

// RTP相關函數
void init_rtp_header(rtp_header_t *hdr, int payload_type, unsigned short seq_num, 
                   unsigned int timestamp, unsigned int ssrc);
void send_rtp_audio(int sockfd, struct sockaddr_in *dest_addr, const char *wav_file, int dest_port,
                  const char *callid, const char *tag, const char *to_tag, const char *cseq,
                  struct sockaddr_in *servaddr);

// RTP接收函數
void* receive_rtp_thread(void *arg);
int start_rtp_receiver(int port, const char *output_filename);
void stop_rtp_receiver(void);

// SIP會話管理
int init_sip_session(sip_session_t *session);
int make_sip_call(sip_session_t *session, const char *callee);
void close_sip_session(sip_session_t *session);

// RTP處理函數
int start_rtp_receiver(int port, const char *output_filename);
void stop_rtp_receiver(void);
void send_rtp_audio(int sockfd, struct sockaddr_in *dest_addr, const char *wav_file, int dest_port,
                   const char *callid, const char *tag, const char *to_tag, const char *cseq,
                   struct sockaddr_in *servaddr);
void init_rtp_header(rtp_header_t *hdr, int payload_type, unsigned short seq_num, 
                    unsigned int timestamp, unsigned int ssrc);

// RTP回調函數類型和設置函數
typedef void (*rtp_data_callback_t)(const unsigned char *rtp_data, size_t data_size);
void set_rtp_callback(rtp_data_callback_t callback);
void clear_rtp_callback(void);
int get_rtp_sockfd(void);  // 獲取當前RTP socket文件描述符

#endif // SIP_CLIENT_H