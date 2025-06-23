#ifndef RTP_RECEIVER_H
#define RTP_RECEIVER_H

#ifdef __cplusplus
extern "C" {
#endif

// 啟動 RTP 接收器
int start_rtp_receiver(int port, const char *output_file);

// 停止 RTP 接收器
void stop_rtp_receiver();

#ifdef __cplusplus
}
#endif

#endif // RTP_RECEIVER_H 