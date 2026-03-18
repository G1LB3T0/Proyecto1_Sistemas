PROTO_ROOT = ../Simple-Chat-Protocol/protos
GEN_DIR    = generated

PROTOS = \
	$(PROTO_ROOT)/common.proto \
	$(PROTO_ROOT)/cliente-side/change_status.proto \
	$(PROTO_ROOT)/cliente-side/get_user_info.proto \
	$(PROTO_ROOT)/cliente-side/list_users.proto \
	$(PROTO_ROOT)/cliente-side/message_dm.proto \
	$(PROTO_ROOT)/cliente-side/message_general.proto \
	$(PROTO_ROOT)/cliente-side/quit.proto \
	$(PROTO_ROOT)/cliente-side/register.proto \
	$(PROTO_ROOT)/server-side/all_users.proto \
	$(PROTO_ROOT)/server-side/broadcast_messages.proto \
	$(PROTO_ROOT)/server-side/for_dm.proto \
	$(PROTO_ROOT)/server-side/get_user_info_response.proto \
	$(PROTO_ROOT)/server-side/server_response.proto

PB_SRCS = \
	$(GEN_DIR)/common.pb.cc \
	$(GEN_DIR)/cliente-side/change_status.pb.cc \
	$(GEN_DIR)/cliente-side/get_user_info.pb.cc \
	$(GEN_DIR)/cliente-side/list_users.pb.cc \
	$(GEN_DIR)/cliente-side/message_dm.pb.cc \
	$(GEN_DIR)/cliente-side/message_general.pb.cc \
	$(GEN_DIR)/cliente-side/quit.pb.cc \
	$(GEN_DIR)/cliente-side/register.pb.cc \
	$(GEN_DIR)/server-side/all_users.pb.cc \
	$(GEN_DIR)/server-side/broadcast_messages.pb.cc \
	$(GEN_DIR)/server-side/for_dm.pb.cc \
	$(GEN_DIR)/server-side/get_user_info_response.pb.cc \
	$(GEN_DIR)/server-side/server_response.pb.cc

CXX      = g++

# Detect protobuf flags via pkg-config (works on Linux and macOS/Homebrew).
# Falls back to bare -lprotobuf if pkg-config is unavailable.
PROTO_CFLAGS := $(shell pkg-config --cflags protobuf 2>/dev/null)
PROTO_LIBS   := $(shell pkg-config --libs   protobuf 2>/dev/null)
ifeq ($(PROTO_LIBS),)
  PROTO_LIBS := -lprotobuf
endif

CXXFLAGS = -std=c++17 -Wall -I$(GEN_DIR) $(PROTO_CFLAGS)
LDFLAGS  = $(PROTO_LIBS) -lpthread

.PHONY: all protos chat_server chat_client clean

all: chat_server chat_client

$(GEN_DIR)/.protos_done: $(PROTOS)
	mkdir -p $(GEN_DIR)/cliente-side $(GEN_DIR)/server-side
	protoc -I $(PROTO_ROOT) --cpp_out=$(GEN_DIR) $(PROTOS)
	touch $(GEN_DIR)/.protos_done

protos: $(GEN_DIR)/.protos_done

chat_server: protos
	$(CXX) $(CXXFLAGS) server/server.cpp $(PB_SRCS) $(LDFLAGS) -o chat_server

chat_client: protos
	$(CXX) $(CXXFLAGS) client/client.cpp $(PB_SRCS) $(LDFLAGS) -o chat_client

clean:
	rm -rf $(GEN_DIR) chat_server chat_client
