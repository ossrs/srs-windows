//
// Copyright (c) 2013-2022 The SRS Authors
//
// SPDX-License-Identifier: MIT or MulanPSL-2.0
//

#ifndef SRS_APP_GB28181_HPP
#define SRS_APP_GB28181_HPP

#include <srs_core.hpp>

#include <srs_app_st.hpp>
#include <srs_app_listener.hpp>
#include <srs_protocol_conn.hpp>
#include <srs_protocol_http_conn.hpp>
#include <srs_kernel_ps.hpp>
#include <srs_app_conn.hpp>

#include <sstream>

class SrsConfDirective;
class SrsTcpListener;
class SrsResourceManager;
class SrsTcpConnection;
class SrsCoroutine;
class SrsPackContext;
class SrsBuffer;
class SrsSipMessage;
class SrsLazyGbSession;
class SrsLazyGbSipTcpConn;
class SrsLazyGbMediaTcpConn;
class SrsLazyGbSipTcpConnWrapper;
class SrsLazyGbMediaTcpConnWrapper;
class SrsLazyGbSipTcpReceiver;
class SrsLazyGbSipTcpSender;
class SrsAlonePithyPrint;
class SrsGbMuxer;
class SrsSimpleRtmpClient;
struct SrsRawAacStreamCodec;
class SrsRawH264Stream;
class SrsSharedPtrMessage;
class SrsPithyPrint;
class SrsRawAacStream;

// The state machine for GB session.
// init:
//      to connecting: sip is registered. Note that also send invite request if media not connected.
// connecting:
//      to established: sip is stable, media is connected.
// established:
//      init: media is not connected.
//      dispose session: sip is bye.
// Please see SrsLazyGbSession::drive_state for detail.
enum SrsGbSessionState
{
    SrsGbSessionStateInit = 0,
    SrsGbSessionStateConnecting,
    SrsGbSessionStateEstablished,
};
std::string srs_gb_session_state(SrsGbSessionState state);

// The state machine for GB SIP connection.
// init:
//      to registered: Got REGISTER SIP message.
//      to stable: Got MESSAGE SIP message, when SIP TCP connection re-connect.
// registered:
//      to inviting: Sent INVITE SIP message.
// inviting:
//      to trying: Got TRYING SIP message.
//      to stable: Got INVITE 200 OK and sent INVITE OK response.
// trying:
//      to stable: Got INVITE 200 OK and sent INVITE OK response.
// stable:
//      to re-inviting: Sent bye to device before re-inviting.
//      to bye: Got bye SIP message from device.
// re-inviting:
//      to inviting: Got bye OK response from deivce.
// Please see SrsLazyGbSipTcpConn::drive_state for detail.
enum SrsGbSipState
{
    SrsGbSipStateInit = 0,
    SrsGbSipStateRegistered,
    SrsGbSipStateInviting,
    SrsGbSipStateTrying,
    SrsGbSipStateReinviting,
    SrsGbSipStateStable,
    SrsGbSipStateBye,
};
std::string srs_gb_sip_state(SrsGbSipState state);

// The interface for GB SIP or HTTP-API connection.
class ISrsGbSipConn
{
public:
    ISrsGbSipConn();
    virtual ~ISrsGbSipConn();
public:
    // Interrupt the transport, because session is disposing.
    virtual void interrupt() {}
    // Get the device id of device, also used as RTMP stream name.
    virtual std::string device_id() { return "livestream"; }
    // Get the state of SIP.
    virtual SrsGbSipState state() { return SrsGbSipStateInit; }
    // Reset the SIP state to registered, for re-inviting.
    virtual void reset_to_register() {}
    // Whether device is already registered, which might drive the session to connecting state.
    virtual bool is_registered() { return false; }
    // Whether device is stable state, which means it's sending heartbeat message.
    virtual bool is_stable() { return false; }
    // Whether device is request to bye, which means there might be no stream ever and so the session should be
    // disposed. This is the control event from client device.
    virtual bool is_bye() { return false; }
    // Send invite to device, for SIP it should be an "INVITE" request message. Output the ssrc as ID of session, for
    // media connection to load from SSRC while receiving and handling RTP packets.
    virtual srs_error_t invite_request(uint32_t* pssrc) { return srs_success; }
    // Change id of coroutine.
    virtual void set_cid(const SrsContextId& cid) {}
};

// The wrapper for ISrsGbSipConn.
class ISrsGbSipConnWrapper
{
private:
    ISrsGbSipConn dummy_;
public:
    ISrsGbSipConnWrapper();
    virtual ~ISrsGbSipConnWrapper();
public:
    virtual ISrsGbSipConn* resource() { return &dummy_; }
    virtual ISrsGbSipConnWrapper* copy() { return new ISrsGbSipConnWrapper(); }
};

// The interface for GB media over TCP or UDP transport.
class ISrsGbMediaConn
{
public:
    ISrsGbMediaConn();
    virtual ~ISrsGbMediaConn();
public:
    // Interrupt the transport, because session is disposing.
    virtual void interrupt() {}
    // Whether media transport is connected. SRS will invite client to publish stream if not connected.
    virtual bool is_connected() { return false; }
    // Change id of coroutine.
    virtual void set_cid(const SrsContextId& cid) {}
};

// The wrapper for ISrsGbMediaConn.
class ISrsGbMediaConnWrapper
{
private:
    ISrsGbMediaConn dummy_;
public:
    ISrsGbMediaConnWrapper();
    virtual ~ISrsGbMediaConnWrapper();
public:
    virtual ISrsGbMediaConn* resource() { return &dummy_; }
    virtual ISrsGbMediaConnWrapper* copy() { return new ISrsGbMediaConnWrapper(); }
};

// The main logic object for GB, the session.
class SrsLazyGbSession : public SrsLazyObject, public ISrsResource, public ISrsStartable, public ISrsCoroutineHandler
{
private:
    SrsCoroutine* trd_;
    SrsContextId cid_;
private:
    SrsGbSessionState state_;
    ISrsGbSipConnWrapper* sip_;
    ISrsGbMediaConnWrapper* media_;
    SrsGbMuxer* muxer_;
private:
    // The candidate for SDP in configuration.
    std::string candidate_;
    // The public IP for SDP, generated by SRS.
    std::string pip_;
    // When wait for SIP and media connecting, timeout if exceed.
    srs_utime_t connecting_starttime_;
    // The max timeout for connecting.
    srs_utime_t connecting_timeout_;
    // The time we enter reinviting state.
    srs_utime_t reinviting_starttime_;
    // The wait time for re-invite.
    srs_utime_t reinvite_wait_;
    // The number of timeout, dispose session if exceed.
    uint32_t nn_timeout_;
private:
    SrsAlonePithyPrint* ppp_;
    srs_utime_t startime_;
    uint64_t total_packs_;
    uint64_t total_msgs_;
    uint64_t total_recovered_;
    uint64_t total_msgs_dropped_;
    uint64_t total_reserved_;
private:
    uint32_t media_id_;
    srs_utime_t media_starttime_;
    uint64_t media_msgs_;
    uint64_t media_packs_;
    uint64_t media_recovered_;
    uint64_t media_msgs_dropped_;
    uint64_t media_reserved_;
private:
    friend class SrsLazyObjectWrapper<SrsLazyGbSession>;
    SrsLazyGbSession();
public:
    virtual ~SrsLazyGbSession();
public:
    // Initialize the GB session.
    srs_error_t initialize(SrsConfDirective* conf);
    // When got a pack of messages.
    void on_ps_pack(SrsPackContext* ctx, SrsPsPacket* ps, const std::vector<SrsTsMessage*>& msgs);
    // When got available SIP transport.
    void on_sip_transport(ISrsGbSipConnWrapper* sip);
    ISrsGbSipConnWrapper* sip_transport();
    // When got available media transport.
    void on_media_transport(ISrsGbMediaConnWrapper* media);
    // Get the candidate for SDP generation, the public IP address for device to connect to.
    std::string pip();
// Interface ISrsStartable
public:
    virtual srs_error_t start();
// Interface ISrsOneCycleThreadHandler
public:
    virtual srs_error_t cycle();
private:
    virtual srs_error_t do_cycle();
    srs_error_t drive_state();
private:
    SrsGbSessionState set_state(SrsGbSessionState v);
// Interface ISrsResource
public:
    virtual const SrsContextId& get_id();
    virtual std::string desc();
};

// Lazy-sweep wrapper for GB session.
class SrsLazyGbSessionWrapper : public ISrsResource
{
    SRS_LAZY_WRAPPER_GENERATOR(SrsLazyGbSession, SrsLazyGbSessionWrapper, SrsLazyGbSession);
};

// The SIP and Media listener for GB.
class SrsGbListener : public ISrsListener, public ISrsTcpHandler
{
private:
    SrsConfDirective* conf_;
    SrsTcpListener* media_listener_;
    SrsTcpListener* sip_listener_;
public:
    SrsGbListener();
    virtual ~SrsGbListener();
public:
    srs_error_t initialize(SrsConfDirective* conf);
    srs_error_t listen();
    void close();
// Interface ISrsTcpHandler
public:
    virtual srs_error_t on_tcp_client(ISrsListener* listener, srs_netfd_t stfd);
};

// A GB28181 TCP SIP connection.
class SrsLazyGbSipTcpConn : public SrsLazyObject, public ISrsResource, public ISrsStartable, public ISrsCoroutineHandler
    , public ISrsGbSipConn
{
private:
    SrsGbSipState state_;
    SrsLazyGbSessionWrapper* session_;
    SrsSipMessage* register_;
    SrsSipMessage* invite_ok_;
private:
    std::string ssrc_str_;
    uint32_t ssrc_v_;
private:
    SrsConfDirective* conf_;
    SrsTcpListener* sip_listener_;
    SrsTcpListener* media_listener_;
private:
    SrsTcpConnection* conn_;
    SrsLazyGbSipTcpReceiver* receiver_;
    SrsLazyGbSipTcpSender* sender_;
    SrsCoroutine* trd_;
private:
    friend class SrsLazyObjectWrapper<SrsLazyGbSipTcpConn>;
    SrsLazyGbSipTcpConn();
public:
    virtual ~SrsLazyGbSipTcpConn();
public:
    // Setup object, to keep empty constructor.
    void setup(SrsConfDirective* conf, SrsTcpListener* sip, SrsTcpListener* media, srs_netfd_t stfd);
    // Get the SIP device id.
    std::string device_id();
    // Set the cid of all coroutines.
    virtual void set_cid(const SrsContextId& cid);
private:
    // Get the sip and media listen port.
    void query_ports(int* sip, int* media);
public:
    // When got a SIP message.
    srs_error_t on_sip_message(SrsSipMessage* msg);
    // Enqueue a SIP message to send, which might be a request or response.
    void enqueue_sip_message(SrsSipMessage* msg);
private:
    void drive_state(SrsSipMessage* msg);
    void register_response(SrsSipMessage* msg);
    void message_response(SrsSipMessage* msg, http_status status);
    void invite_ack(SrsSipMessage* msg);
    void bye_response(SrsSipMessage* msg);
public:
    srs_error_t invite_request(uint32_t* pssrc);
public:
    // Interrupt transport by session.
    void interrupt();
    // Get the SIP state.
    SrsGbSipState state();
    // Reset the SIP state to registered, for re-inviting.
    void reset_to_register();
    // Whether SIP state is registered or more.
    bool is_registered();
    // Whether SIP state is stable state, established.
    bool is_stable();
    // Whether SIP is bye bye.
    bool is_bye();
private:
    SrsGbSipState set_state(SrsGbSipState v);
// Interface ISrsResource
public:
    virtual const SrsContextId& get_id();
    virtual std::string desc();
// Interface ISrsStartable
public:
    virtual srs_error_t start();
// Interface ISrsOneCycleThreadHandler
public:
    virtual srs_error_t cycle();
private:
    virtual srs_error_t do_cycle();
private:
    // Create session if no one, or bind to an existed session.
    srs_error_t bind_session(SrsSipMessage* msg, SrsLazyGbSessionWrapper** psession);
};

// Lazy-sweep wrapper for GB SIP TCP connection.
class SrsLazyGbSipTcpConnWrapper : public ISrsResource, public ISrsGbSipConnWrapper
{
    SRS_LAZY_WRAPPER_GENERATOR(SrsLazyGbSipTcpConn, ISrsGbSipConnWrapper, ISrsGbSipConn);
};

// Start a coroutine to receive SIP messages.
class SrsLazyGbSipTcpReceiver : public ISrsStartable, public ISrsCoroutineHandler
{
private:
    SrsCoroutine* trd_;
    SrsTcpConnection* conn_;
    SrsLazyGbSipTcpConn* sip_;
public:
    SrsLazyGbSipTcpReceiver(SrsLazyGbSipTcpConn* sip, SrsTcpConnection* conn);
    virtual ~SrsLazyGbSipTcpReceiver();
public:
    // Interrupt the receiver coroutine.
    void interrupt();
    // Set the cid of all coroutines.
    virtual void set_cid(const SrsContextId& cid);
// Interface ISrsStartable
public:
    virtual srs_error_t start();
// Interface ISrsOneCycleThreadHandler
public:
    virtual srs_error_t cycle();
private:
    srs_error_t do_cycle();
};

// Start a coroutine to send out SIP messages.
class SrsLazyGbSipTcpSender : public ISrsStartable, public ISrsCoroutineHandler
{
private:
    SrsCoroutine* trd_;
    SrsTcpConnection* conn_;
private:
    std::vector<SrsSipMessage*> msgs_;
    srs_cond_t wait_;
public:
    SrsLazyGbSipTcpSender(SrsTcpConnection* conn);
    virtual ~SrsLazyGbSipTcpSender();
public:
    // Push message to queue, and sender will send out in dedicate coroutine.
    void enqueue(SrsSipMessage* msg);
    // Interrupt the sender coroutine.
    void interrupt();
    // Set the cid of all coroutines.
    virtual void set_cid(const SrsContextId& cid);
// Interface ISrsStartable
public:
    virtual srs_error_t start();
// Interface ISrsOneCycleThreadHandler
public:
    virtual srs_error_t cycle();
private:
    srs_error_t do_cycle();
};

// The handler for a pack of PS PES packets.
class ISrsPsPackHandler
{
public:
    ISrsPsPackHandler();
    virtual ~ISrsPsPackHandler();
public:
    // When got a pack of PS/TS messages, contains one video frame and optional SPS/PPS message, one or generally more
    // audio frames. Note that the ps contains the pack information.
    virtual srs_error_t on_ps_pack(SrsPsPacket* ps, const std::vector<SrsTsMessage*>& msgs) = 0;
};

// A GB28181 TCP media connection, for PS stream.
class SrsLazyGbMediaTcpConn : public SrsLazyObject, public ISrsResource, public ISrsStartable, public ISrsCoroutineHandler
    , public ISrsPsPackHandler, public ISrsGbMediaConn
{
private:
    bool connected_;
    SrsLazyGbSessionWrapper* session_;
    uint32_t nn_rtcp_;
private:
    SrsPackContext* pack_;
    SrsTcpConnection* conn_;
    SrsCoroutine* trd_;
    uint8_t* buffer_;
private:
    friend class SrsLazyObjectWrapper<SrsLazyGbMediaTcpConn>;
    SrsLazyGbMediaTcpConn();
public:
    virtual ~SrsLazyGbMediaTcpConn();
public:
    // Setup object, to keep empty constructor.
    void setup(srs_netfd_t stfd);
    // Whether media is connected.
    bool is_connected();
    // Interrupt transport by session.
    void interrupt();
    // Set the cid of all coroutines.
    virtual void set_cid(const SrsContextId& cid);
// Interface ISrsResource
public:
    virtual const SrsContextId& get_id();
    virtual std::string desc();
// Interface ISrsStartable
public:
    virtual srs_error_t start();
// Interface ISrsOneCycleThreadHandler
public:
    virtual srs_error_t cycle();
private:
    virtual srs_error_t do_cycle();
// Interface ISrsPsPackHandler
public:
    virtual srs_error_t on_ps_pack(SrsPsPacket* ps, const std::vector<SrsTsMessage*>& msgs);
private:
    // Create session if no one, or bind to an existed session.
    srs_error_t bind_session(uint32_t ssrc, SrsLazyGbSessionWrapper** psession);
};

// Lazy-sweep wrapper for GB Media TCP connection.
class SrsLazyGbMediaTcpConnWrapper : public ISrsResource, public ISrsGbMediaConnWrapper
{
    SRS_LAZY_WRAPPER_GENERATOR(SrsLazyGbMediaTcpConn, ISrsGbMediaConnWrapper, ISrsGbMediaConn);
};

// The queue for mpegts over udp to send packets.
// For the aac in mpegts contains many flv packets in a pes packet,
// we must recalc the timestamp.
class SrsMpegpsQueue
{
private:
    // The key: dts, value: msg.
    std::map<int64_t, SrsSharedPtrMessage*> msgs;
    int nb_audios;
    int nb_videos;
public:
    SrsMpegpsQueue();
    virtual ~SrsMpegpsQueue();
public:
    virtual srs_error_t push(SrsSharedPtrMessage* msg);
    virtual SrsSharedPtrMessage* dequeue();
};

// Mux GB28181 to RTMP.
class SrsGbMuxer
{
private:
    SrsLazyGbSession* session_;
    std::string output_;
    SrsSimpleRtmpClient* sdk_;
private:
    SrsRawH264Stream* avc_;
    std::string h264_sps_;
    bool h264_sps_changed_;
    std::string h264_pps_;
    bool h264_pps_changed_;
    bool h264_sps_pps_sent_;
private:
    SrsRawAacStream* aac_;
    std::string aac_specific_config_;
private:
    SrsMpegpsQueue* queue_;
    SrsPithyPrint* pprint_;
public:
    SrsGbMuxer(SrsLazyGbSession* session);
    virtual ~SrsGbMuxer();
public:
    srs_error_t initialize(std::string output);
    srs_error_t on_ts_message(SrsTsMessage* msg);
private:
    virtual srs_error_t on_ts_video(SrsTsMessage* msg, SrsBuffer* avs);
    virtual srs_error_t write_h264_sps_pps(uint32_t dts, uint32_t pts);
    virtual srs_error_t write_h264_ipb_frame(char* frame, int frame_size, uint32_t dts, uint32_t pts);
    virtual srs_error_t on_ts_audio(SrsTsMessage* msg, SrsBuffer* avs);
    virtual srs_error_t write_audio_raw_frame(char* frame, int frame_size, SrsRawAacStreamCodec* codec, uint32_t dts);
    virtual srs_error_t rtmp_write_packet(char type, uint32_t timestamp, char* data, int size);
private:
    // Connect to RTMP server.
    virtual srs_error_t connect();
    // Close the connection to RTMP server.
    virtual void close();
};

// Response writer for SIP.
class SrsSipResponseWriter : public SrsHttpResponseWriter
{
public:
    SrsSipResponseWriter(ISrsProtocolReadWriter* io);
    virtual ~SrsSipResponseWriter();
// Interface ISrsHttpFirstLineWriter
public:
    virtual srs_error_t build_first_line(std::stringstream& ss, char* data, int size);
};

// Request writer for SIP.
class SrsSipRequestWriter : public SrsHttpRequestWriter
{
public:
    SrsSipRequestWriter(ISrsProtocolReadWriter* io);
    virtual ~SrsSipRequestWriter();
// Interface ISrsHttpFirstLineWriter
public:
    virtual srs_error_t build_first_line(std::stringstream& ss, char* data, int size);
};

// SIP message, covert with HTTP message.
class SrsSipMessage
{
public:
    // SIP message type, request or response.
    http_parser_type type_;
    // For SIP request.
    http_method method_; // For example: HTTP_INVITE
    std::string request_uri_; // For example: sip:34020000001320000001@3402000000
    std::string request_uri_user_; // For example: 34020000001320000001
    std::string request_uri_host_; // For example: 3402000000
    // For SIP response.
    http_status status_; // For example: HTTP_STATUS_OK
public:
    // The Via header field indicates the path taken by the request so far and indicates the path that should be
    // followed in routing responses. See https://www.ietf.org/rfc/rfc3261.html#section-20.42
    // @param transport TCP or UDP, the transport protocol.
    // @param send_by The host and port which send the packet.
    // @param branch Transaction identifier, see https://www.ietf.org/rfc/rfc3261.html#section-8.1.1.7
    // @param rport For UDP to traverse NATs, see https://www.ietf.org/rfc/rfc3581.html
    std::string via_; // For example: SIP/2.0/TCP 192.168.3.82:5060;rport;branch=z9hG4bK0l31rx
    std::string via_transport_; // For example: TCP
    std::string via_send_by_; // For example: 192.168.3.82:5060
    std::string via_send_by_address_; // For example: 192.168.3.82
    int via_send_by_port_; // For example: 5060
    std::string via_branch_; // For example: branch=z9hG4bK0l31rx
    std::string via_rport_; // For example: rport
public:
    // See https://www.ietf.org/rfc/rfc3261.html#section-20.20
    // See https://www.ietf.org/rfc/rfc3261.html#section-8.1.1.3
    std::string from_; // For example: <sip:34020000002000000001@3402000000>;tag=SRSk1er282t
    std::string from_address_; // For example: <sip:34020000002000000001@3402000000>
    std::string from_address_user_; // For example: 34020000002000000001
    std::string from_address_host_; // For example: 3402000000
    std::string from_tag_; // For example: tag=SRSk1er282t
public:
    // See https://www.ietf.org/rfc/rfc3261.html#section-8.1.1.2
    // See https://www.ietf.org/rfc/rfc3261.html#section-20.39
    std::string to_; // For example: <sip:34020000001320000001@3402000000>;tag=SRSk1er282t
    std::string to_address_; // For example: <sip:34020000001320000001@3402000000>
    std::string to_address_user_; // For example: 34020000001320000001
    std::string to_address_host_; // For example: 3402000000
    std::string to_tag_; // For example: tag=SRSk1er282t
public:
    // See https://www.ietf.org/rfc/rfc3261.html#section-8.1.1.4
    // See https://www.ietf.org/rfc/rfc3261.html#section-20.8
    std::string call_id_; // For example: 854k7337207yxpfj
    // See https://www.ietf.org/rfc/rfc3261.html#section-8.1.1.8
    // See https://www.ietf.org/rfc/rfc3261.html#section-20.10
    std::string contact_; // For example: <sip:34020000002000000001@3402000000>
    std::string contact_user_; // For example: <sip:34020000002000000001@3402000000>
    std::string contact_host_; // For example: 3402000000
    std::string contact_host_address_; // For example: 3402000000
    int contact_host_port_; // For example: 5060
    // See https://www.ietf.org/rfc/rfc3261.html#section-20.19
    uint32_t expires_;
    // See https://www.ietf.org/rfc/rfc3261.html#section-8.1.1.6
    // See https://www.ietf.org/rfc/rfc3261.html#section-20.22
    uint32_t max_forwards_;
public:
    // See https://www.ietf.org/rfc/rfc3261.html#section-8.1.1.5
    // See https://www.ietf.org/rfc/rfc3261.html#section-20.16
    std::string cseq_;
    uint32_t cseq_number_;
    std::string cseq_method_;
public:
    // See https://openstd.samr.gov.cn/bzgk/gb/newGbInfo?hcno=469659DC56B9B8187671FF08748CEC89
    std::string subject_;
    // The content type.
    std::string content_type_;
public:
    // SIP message body.
    std::string body_;
    // Escape \r to \\r, \n to \\n.
    std::string body_escaped_;
public:
    SrsSipMessage();
    virtual ~SrsSipMessage();
public:
    SrsSipMessage* copy();
    const std::string& device_id();
    std::string ssrc_domain_id();
    SrsSipMessage* set_body(std::string v);
    srs_error_t parse(ISrsHttpMessage* m);
private:
    srs_error_t parse_via(const std::string& via);
    srs_error_t parse_from(const std::string& from);
    srs_error_t parse_to(const std::string& to);
    srs_error_t parse_cseq(const std::string& cseq);
    srs_error_t parse_contact(const std::string& contact);
public:
    bool is_register() {
        return type_ == HTTP_REQUEST && method_ == HTTP_REGISTER;
    }
    bool is_message() {
        return type_ == HTTP_REQUEST && method_ == HTTP_MESSAGE;
    }
    bool is_invite() {
        return type_ == HTTP_REQUEST && method_ == HTTP_INVITE;
    }
    bool is_trying() {
        return type_ == HTTP_RESPONSE && cseq_method_ == "INVITE" && status_ == HTTP_STATUS_CONTINUE;
    }
    bool is_invite_ok() {
        return type_ == HTTP_RESPONSE && cseq_method_ == "INVITE" && status_ == HTTP_STATUS_OK;
    }
    bool is_bye() {
        return type_ == HTTP_REQUEST && method_ == HTTP_BYE;
    }
    bool is_bye_ok() {
        return type_ == HTTP_RESPONSE && cseq_method_ == "BYE" && status_ == HTTP_STATUS_OK;
    }
};

// Recoverable PS context for GB28181.
class SrsRecoverablePsContext
{
public:
    SrsPsContext ctx_;
private:
    // If decoding error, enter the recover mode. Drop all left bytes util next pack header.
    int recover_;
public:
    SrsRecoverablePsContext();
    virtual ~SrsRecoverablePsContext();
public:
    // Decode the PS stream in RTP payload. Note that there might be first reserved bytes of RAW data, which is not
    // parsed by previous decoding, we should move to the start of payload bytes.
    virtual srs_error_t decode_rtp(SrsBuffer* stream, int reserved, ISrsPsMessageHandler* handler);
private:
    // Decode the RTP payload as PS pack stream.
    virtual srs_error_t decode(SrsBuffer* stream, ISrsPsMessageHandler* handler);
    // When got error, drop data and enter recover mode.
    srs_error_t enter_recover_mode(SrsBuffer* stream, ISrsPsMessageHandler* handler, int pos, srs_error_t err);
    // Quit Recover mode when got pack header.
    void quit_recover_mode(SrsBuffer* stream, ISrsPsMessageHandler* handler);
};

// The PS pack context, for GB28181 to process based on PS pack, which contains a video and audios messages. For large
// video frame, it might be split to multiple PES packets, which must be group to one video frame.
// Please note that a pack might contain multiple audio frames, so size of audio PES packet should not exceed 64KB,
// which is limited by the 16 bits PES_packet_length.
// We also correct the timestamp, or DTS/PTS of video frames, which might be 0 if more than one video PES packets in a
// PS pack stream.
class SrsPackContext : public ISrsPsMessageHandler
{
public:
    // Each media transport only use one context, so the context id is the media id.
    uint32_t media_id_;
    srs_utime_t media_startime_;
    uint64_t media_nn_recovered_;
    uint64_t media_nn_msgs_dropped_;
    uint64_t media_reserved_;
private:
    // To process a pack of TS/PS messages.
    ISrsPsPackHandler* handler_;
    // Note that it might be freed, so never use its fields.
    SrsPsPacket* ps_;
    // The messages in current PS pack.
    std::vector<SrsTsMessage*> msgs_;
public:
    SrsPackContext(ISrsPsPackHandler* handler);
    virtual ~SrsPackContext();
private:
    void clear();
// Interface ISrsPsMessageHandler
public:
    virtual srs_error_t on_ts_message(SrsTsMessage* msg);
    virtual void on_recover_mode(int nn_recover);
};

// Find the pack header which starts with bytes (00 00 01 ba).
extern bool srs_skip_util_pack(SrsBuffer* stream);

// Parse the address from SIP string, for example:
//      sip:bob@ossrs.io:5060
//      <sip:bob@ossrs.io:5060>
//      Bob <sip:bob@ossrs.io:5060>
// Parsed as:
//      user: bob
//      host: ossrs.io:5060
// Note that the host can be parsed by srs_parse_hostport as host(ossrs.io) and port(5060).
extern void srs_sip_parse_address(const std::string& address, std::string& user, std::string& host);

// Manager for GB connections.
extern SrsResourceManager* _srs_gb_manager;

#endif

