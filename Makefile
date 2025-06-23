CC = gcc
CFLAGS = -Wall -g -O2 -I.
LDFLAGS = -lssl -lcrypto -lpthread -lm

# 源文件
LIB_SRCS = lib/sip_client.c lib/sip_message.c lib/rtp.c lib/sip_call.c
DEMO_SRC = sip_client_demo.c

# 目標文件
LIB_OBJS = $(LIB_SRCS:.c=.o)
DEMO_OBJ = $(DEMO_SRC:.c=.o)

# 目標執行檔
DEMO = sip_client_demo

# 默認目標
all: $(DEMO)

# 編譯庫的目標
lib: $(LIB_OBJS)

# 編譯SIP客戶端示範程式
$(DEMO): $(DEMO_OBJ) $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 清理生成的文件
clean:
	rm -f $(LIB_OBJS) $(DEMO_OBJ) $(DEMO)

# 編譯規則
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 依賴關係
$(DEMO_OBJ): lib/sip_client.h
$(LIB_OBJS): lib/sip_client.h

.PHONY: all clean lib 