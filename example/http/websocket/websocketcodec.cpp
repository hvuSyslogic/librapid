//---------------------------------------------------------------------------------------------------------------------
// Copyright (c) 2015-2016 librapid project. All rights reserved.
// More license information, please see LICENSE file in module root folder.
//---------------------------------------------------------------------------------------------------------------------

#include <iterator>

#include <rapid/iobuffer.h>
#include <rapid/logging/logging.h>

#include <rapid/details/common.h>
#include <rapid/utils/singleton.h>
#include <rapid/utils/stringutilis.h>
#include <rapid/utils/byteorder.h>
#include <rapid/utilis.h>

#include "../openssl/openssl.h"

#include "../httpserverconfigfacade.h"
#include "websocketcodec.h"

std::string getWebsocketAcceptKey(std::string const &key) {
	auto const sha1 = getSHA1(key + WS_GUID);
	std::string sha1Str;
	sha1Str.reserve(sha1.size());
	std::copy(sha1.begin(), sha1.end(), std::back_inserter(sha1Str));
	return encodeBase64(sha1Str);
}

/*

 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
|     Extended payload length continued, if payload len == 127  |
+ - - - - - - - - - - - - - - - +-------------------------------+
|                               |Masking-key, if MASK set to 1  |
+-------------------------------+-------------------------------+
| Masking-key (continued)       |          Payload Data         |
+-------------------------------- - - - - - - - - - - - - - - - +
:                     Payload Data continued ...                :
+ - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
|                     Payload Data continued ...                |
+---------------------------------------------------------------+

*/
static void writeWebSocketHeader(rapid::IoBuffer* pBuffer, uint64_t contentLength) {
	pBuffer->makeWriteableSpace(WS_MIN_SIZE);

	auto extendedLength = 0;

	uint8_t byteFirst = WS_MASK_FIN | WebSocketOpcodes::WS_OP_TEXT;
	rapid::writeData(pBuffer, byteFirst);

	// Not set mask bit, browser don't support mask.
	uint8_t byteSecond = 0x00;

	if (contentLength <= WS_NO_EXT_PAYLOAD_LENGTH_MAX) {
		byteSecond = contentLength;
	} else if (contentLength <= UINT16_MAX) {
		byteSecond |= WS_16BIT_EXT_PAYLOAD_LENGTH;
		extendedLength = sizeof(uint16_t);
	} else {
		byteSecond |= WS_64BIT_EXT_PAYLOAD_LENGTH;
		extendedLength = 8;
	}

	rapid::writeData(pBuffer, byteSecond);

	if (extendedLength == sizeof(uint16_t)) {
		pBuffer->makeWriteableSpace(sizeof(uint16_t));
		rapid::writeData<rapid::utils::BigEndian>(pBuffer, static_cast<uint16_t>(contentLength));
	} else if (extendedLength == sizeof(uint64_t)) {
		pBuffer->makeWriteableSpace(sizeof(uint64_t));
		rapid::writeData<rapid::utils::BigEndian>(pBuffer, contentLength);
	}
}

WebSocketRequest::WebSocketRequest()
	: opcode_(WebSocketOpcodes::WS_OP_CLOSED) {
}

WebSocketRequest::~WebSocketRequest() noexcept {
}

bool WebSocketRequest::isClosed() const noexcept {
	return opcode_ == WS_OP_CLOSED;
}

void WebSocketRequest::setOpcode(WebSocketOpcodes opCode) noexcept {
	opcode_ = opCode;
}

bool WebSocketRequest::isTextFormat() const noexcept {
	return opcode_ == WS_OP_TEXT;
}

bool WebSocketRequest::isBinaryFormat() const noexcept {
	return opcode_ == WS_OP_BINARY;
}

bool WebSocketRequest::isPing() const noexcept {
	return opcode_ == WS_OP_PING;
}

bool WebSocketRequest::isPong() const noexcept {
	return opcode_ == WS_OP_PONG;
}

uint64_t WebSocketRequest::getConetentLength() const noexcept {
	return content_.length();
}

std::string WebSocketRequest::content() const {
	return content_;
}

void WebSocketRequest::setContent(std::string const &content) {
	content_ = content;
}

void WebSocketRequest::doSerialize(rapid::IoBuffer* pBuffer) {
}

void WebSocketResponse::doSerialize(rapid::IoBuffer* pBuffer) {
	writeWebSocketHeader(pBuffer, contentLength_);
}

WebSocketResponse::WebSocketResponse()
	: contentLength_(0) {
}

WebSocketResponse::~WebSocketResponse() {
}

void WebSocketResponse::setContentLength(uint64_t length) {
	contentLength_ = length;
}

WebSocketFrameReader::WebSocketFrameReader()
	: lastFrame_(true)
	, isMasked_(true)
	, unparsedContentLength_(0)
	, opcode_(WS_OP_CLOSED)
	, state_(WS_PARSE_FIN)
	, indexOfFirstMask_(0)
	, bytesReceiveLengthAndMask_(0)
	, contentLength_(0)
	, totalReadBytes_(0) {
}

void WebSocketFrameReader::reset() noexcept {
	lastFrame_ = true;
	isMasked_ = true;
	state_ = WS_PARSE_FIN;
	unparsedContentLength_ = 0;
	opcode_ = WS_OP_CONTINUATION;
	bytesReceiveLengthAndMask_ = 0;
	contentLength_ = 0;
	indexOfFirstMask_ = 0;
	totalReadBytes_ = 0;
}

WebSocketOpcodes WebSocketFrameReader::opcode() const noexcept {
	return opcode_;
}

void WebSocketFrameReader::parseFinAndContentLength(rapid::IoBuffer* pBuffer) {
	auto firstByte = pBuffer->peek()[0];
	auto sencondByte = pBuffer->peek()[1];

	lastFrame_ = (firstByte & WS_MASK_FIN) == WS_MASK_FIN;
	unparsedContentLength_ = (sencondByte & WS_MASK_PAYLOAD_LENGTH);
	isMasked_ = (sencondByte & 0x80) >> 7;
	RAPID_ENSURE(isMasked_);

	opcode_ = static_cast<WebSocketOpcodes>(firstByte & WS_MASK_OPCODE);

	switch (opcode_) {
	case WS_OP_CONTINUATION:
	case WS_OP_TEXT:
	case WS_OP_BINARY:
	case WS_OP_CLOSED:
	case WS_OP_PING:
	case WS_OP_PONG:
		break;
	default:
		RAPID_ENSURE("Error opcode" && 0);
	}
}

bool WebSocketFrameReader::isReadyRead(rapid::IoBuffer* pBuffer) {
	size_t bytesRead = 0;

	switch (unparsedContentLength_) {
		// �]�t16 bit Extend Payload Length
	case WS_16BIT_EXT_PAYLOAD_LENGTH:
		bytesRead = (WS_MIN_SIZE + getMaskSize() + 2);
		break;
		// �]�t 64 bit Extend Payload Length
	case WS_64BIT_EXT_PAYLOAD_LENGTH:
		bytesRead = (WS_MIN_SIZE + getMaskSize() + 8);
		break;
	default:
		bytesRead = WS_MIN_SIZE + getMaskSize();
		break;
	}

	if (pBuffer->readable() >= bytesRead) {
		return true;
	}

	bytesReceiveLengthAndMask_ = bytesRead - pBuffer->readable();
	return false;
}

void WebSocketFrameReader::parseContentLength(rapid::IoBuffer* pBuffer) {
	indexOfFirstMask_ = 2;

	switch (unparsedContentLength_) {
		// �]�t16 bit Extend Payload Length
	case WS_16BIT_EXT_PAYLOAD_LENGTH:
		memcpy(&contentLength_, pBuffer->peek() + WS_MIN_SIZE, sizeof(uint16_t));
		contentLength_ = static_cast<uint64_t>(ntohs(contentLength_));
		indexOfFirstMask_ = 4;
		bytesReceiveLengthAndMask_ = getMaskSize() + sizeof(uint16_t);
		break;
	case WS_64BIT_EXT_PAYLOAD_LENGTH:
		// �]�t 64 bit Extend Payload Length
		memcpy(&contentLength_, pBuffer->peek() + WS_MIN_SIZE, sizeof(uint64_t));
		contentLength_ = htonll(contentLength_);
		indexOfFirstMask_ = 10;
		bytesReceiveLengthAndMask_ = getMaskSize() + sizeof(uint64_t);
		break;
	default:
		bytesReceiveLengthAndMask_ = getMaskSize();
		contentLength_ = unparsedContentLength_;
		break;
	}

	RAPID_ENSURE(isMasked_);
	memcpy(mask_, pBuffer->peek() + indexOfFirstMask_, WS_MASK_SIZE);
}

void WebSocketFrameReader::maskData(rapid::IoBuffer *buffer) {
	RAPID_TRACE_CALL();
	RAPID_ENSURE(isMasked_);
	for (uint64_t i = 0; i < contentLength_; ++i) {
		buffer->peek()[i] = buffer->peek()[i] ^ mask_[i % 4];
	}
}

size_t WebSocketFrameReader::getMaskSize() const noexcept {
	return !isMasked_ ? 0 : 4;
}

uint64_t WebSocketFrameReader::getConetentLength() const noexcept {
	return contentLength_;
}

void WebSocketFrameReader::readFrame(rapid::IoBuffer* buffer, uint32_t &wantReadSize) {
	RAPID_TRACE_CALL();
	switch (state_) {
	case WS_PARSE_FIN:
		if (buffer->isEmpty()) {
			wantReadSize = WS_MIN_SIZE;
			break;
		}
		parseFinAndContentLength(buffer);
		state_ = WS_PARSE_EXPECTED_SIZE;
	case WS_PARSE_EXPECTED_SIZE:
		if (!isReadyRead(buffer)) {
			wantReadSize = bytesReceiveLengthAndMask_;
			break;
		} else {
			parseContentLength(buffer);
			buffer->retrieve(WS_MIN_SIZE + bytesReceiveLengthAndMask_);
			wantReadSize = contentLength_;
			state_ = WS_PARSE_READ_DATA;
		}
	case WS_PARSE_READ_DATA:
		wantReadSize = contentLength_ - (std::min)(contentLength_, static_cast<uint64_t>(buffer->readable()));
		if (wantReadSize > 0) {
			break;
		}
		state_ = WS_PARSE_DONE;
	case WS_PARSE_DONE:
		maskData(buffer);
		wantReadSize = 0;
		break;
	}
}

WebSocketCodec::WebSocketCodec(std::shared_ptr<MessageDispatcher<WebSocketRequest>> disp)
	: dispatcher_(disp) {
	pWebSocketRequest_ = std::make_shared<WebSocketRequest>();
}

void WebSocketCodec::readLoop(rapid::ConnectionPtr &pConn, uint32_t &bytesToRead) {
	RAPID_TRACE_CALL();

	auto pBuffer = pConn->getReceiveBuffer();
	
	do {
		reader_.readFrame(pBuffer, bytesToRead);
		if (!bytesToRead) {
			pWebSocketRequest_->setOpcode(reader_.opcode());
			pWebSocketRequest_->setContent(pBuffer->read(reader_.getConetentLength()));
			reader_.reset();
			dispatcher_->onMessage(WS_MESSAGE, pConn, pWebSocketRequest_);			
		}
	} while (!bytesToRead && !pBuffer->isEmpty());
	
	/*
	reader_.readFrame(pBuffer, bytesToRead);
	if (!bytesToRead) {
		pWebSocketRequest_->setOpcode(reader_.opcode());
		pWebSocketRequest_->setContent(pBuffer->read(reader_.getConetentLength()));
		bool isEmpty = pBuffer->isEmpty();
		dispatcher_->onMessage(WS_MESSAGE, pConn, pWebSocketRequest_);
		reader_.reset();
	}
	*/
}