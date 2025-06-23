#include "lib/sip_client.h"
#include <termios.h>
#include <signal.h>

/**
 * 接收對方通話RTP音檔的工具
 * 使用 sip/lib 下的函數而不修改它們
 */

// 全局變量用於信號處理
volatile sig_atomic_t end_call = 0;

// 信號處理函數
void signal_handler(int sig) {
    if (sig == SIGINT) {
        log_with_timestamp("接收到中斷信號，準備結束通話...\n");
        end_call = 1;
        
        // 强制清除終端輸入模式
        struct termios oldt;
        tcgetattr(STDIN_FILENO, &oldt);
        oldt.c_lflag |= (ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    }
}

// 使用替代方法監聽按鍵
void wait_for_key_or_timeout(int timeout_seconds) {
    log_with_timestamp("通話已建立，正在記錄音頻. 按Enter鍵或任意鍵結束通話...\n");
    log_with_timestamp("或者等待 %d 秒後自動結束...\n", timeout_seconds);
    
    // 設置終端為非規範模式
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    
    // 設置選擇超時
    fd_set readfds;
    struct timeval tv;
    int ret;
    
    // 設置處理Ctrl+C的信號處理器
    signal(SIGINT, signal_handler);
    
    // 等待輸入或超時
    time_t start_time = time(NULL);
    
    // 確保通話至少維持一段時間，以便有時間處理音頻
    int min_duration = 5; // 最少等待5秒以確保有足夠的時間接收音頻
    time_t min_end_time = start_time + min_duration;
    
    log_with_timestamp("確保通話至少持續 %d 秒以接收足夠的音頻數據\n", min_duration);
    
    while (!end_call) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        
        // 計算剩餘時間
        time_t current_time = time(NULL);
        int elapsed = current_time - start_time;
        
        // 檢查是否達到最小持續時間
        if (current_time < min_end_time) {
            // 如果還沒達到最小持續時間，即使有按鍵輸入也繼續等待
            tv.tv_sec = 0;
            tv.tv_usec = 500000;  // 等待0.5秒
        } else if (elapsed >= timeout_seconds) {
            // 達到最大通話時間
            log_with_timestamp("達到最大通話時間，結束通話\n");
            break;
        } else {
            // 正常檢查按鍵輸入
            tv.tv_sec = 0;
            tv.tv_usec = 500000;  // 0.5秒
        }
        
        ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
        
        if (ret == -1) {
            // 發生錯誤
            if (errno != EINTR) {  // 忽略被信號中斷的情況
                log_with_timestamp("select錯誤: %s\n", strerror(errno));
                break;
            }
        } else if (ret > 0 && current_time >= min_end_time) {
            // 有輸入可用且已達到最小持續時間
            if (FD_ISSET(STDIN_FILENO, &readfds)) {
                char c;
                if (read(STDIN_FILENO, &c, 1) > 0) {
                    log_with_timestamp("檢測到按鍵輸入，結束通話\n");
                    break;
                }
            }
        }
        
        // 額外檢查中斷標誌
        if (end_call && current_time >= min_end_time) {
            log_with_timestamp("檢測到中斷信號，準備結束...\n");
            break;
        }
        
        // 每隔幾秒報告通話正在進行
        if (elapsed > 0 && elapsed % 5 == 0) {
            log_with_timestamp("通話進行中: %d 秒已經過...\n", elapsed);
        }
    }
    
    // 恢復終端設置
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
}

int main(int argc, char *argv[]) {
    sip_session_t session;
    const char *callee = CALLEE;   // 使用默認被叫號碼
    const char *output_file = "received_audio.wav";  // 默認輸出文件名
    int timeout_seconds = 120;  // 默認通話時間限制為120秒
    
    // 初始化隨機數種子
    srand(time(NULL));
    
    // 設置信號處理器以便從任何地方都能正確處理Ctrl+C
    signal(SIGINT, signal_handler);
    
    log_with_timestamp("RTP音頻接收器啟動\n");
    
    // 處理命令行參數
    if (argc > 1) {
        callee = argv[1];
    }
    
    if (argc > 2) {
        output_file = argv[2];
    }
    
    if (argc > 3) {
        timeout_seconds = atoi(argv[3]);
        if (timeout_seconds <= 0) {
            timeout_seconds = 120;  // 如果輸入無效，使用默認值
        }
    }
    
    log_with_timestamp("被叫號碼: %s, 輸出文件: %s, 最大通話時間: %d秒\n", 
                    callee, output_file, timeout_seconds);
    
    // 初始化SIP會話
    if (init_sip_session(&session) != 0) {
        log_with_timestamp("初始化SIP會話失敗\n");
        return 1;
    }
    
    // 發起SIP呼叫 - 這會等待收到 200 OK 響應
    log_with_timestamp("發起呼叫到 %s\n", callee);
    if (make_sip_call(&session, callee) != 0) {
        log_with_timestamp("呼叫失敗\n");
        close_sip_session(&session);
        return 1;
    }
    
    log_with_timestamp("呼叫建立成功！遠端RTP端口: %d\n", session.remote_rtp_port);
    log_with_timestamp("通話已接通，現在開始啟動RTP接收器...\n");
    
    // 在接通電話後才啟動RTP接收器
    if (start_rtp_receiver(LOCAL_RTP_PORT, output_file) != 0) {
        log_with_timestamp("啟動RTP接收器失敗\n");
        close_sip_session(&session);
        return 1;
    }
    
    // 暫停一小段時間，確保RTP流有機會建立
    log_with_timestamp("等待2秒讓RTP流建立...\n");
    sleep(2);
    
    // 等待用戶輸入或超時
    wait_for_key_or_timeout(timeout_seconds);
    
    // 停止RTP接收
    log_with_timestamp("正在停止RTP接收...\n");
    stop_rtp_receiver();
    
    // 關閉SIP會話
    log_with_timestamp("正在關閉SIP會話...\n");
    close_sip_session(&session);
    
    log_with_timestamp("通話結束，音頻已保存到 %s\n", output_file);
    return 0;
} 