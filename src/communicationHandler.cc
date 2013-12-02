/**
 * @file communicationHandler.cc
 * @author Rafal Slota
 * @copyright (C) 2013 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in 'LICENSE.txt'
 */

#include "communicationHandler.h"
#include "fuse_messages.pb.h"
#include "glog/logging.h"
#include "helpers/storageHelperFactory.h"
#include <google/protobuf/descriptor.h>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

using namespace std;
using namespace boost;
using namespace veil::protocol::communication_protocol;
using namespace veil::protocol::fuse_messages;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

namespace veil {

volatile int CommunicationHandler::instancesCount = 0;
boost::mutex CommunicationHandler::m_instanceMutex;

CommunicationHandler::CommunicationHandler(string hostname, int port, string certPath)
    : m_hostname(hostname),
      m_port(port),
      m_certPath(certPath),
      m_connectStatus(CLOSED),
      m_currentMsgId(1),
      m_errorCount(0),
      m_isPushChannel(false)
{
    boost::unique_lock<boost::mutex> lock(m_instanceMutex);
    ++instancesCount;
}
    
    
CommunicationHandler::~CommunicationHandler()
{
    closeConnection();
    boost::unique_lock<boost::mutex> lock(m_instanceMutex);
    --instancesCount;
}
    
unsigned int CommunicationHandler::getErrorCount()
{
    return m_errorCount;
}

void CommunicationHandler::setFuseID(string fuseId) 
{
    m_fuseID = fuseId;
}

void CommunicationHandler::setPushCallback(push_callback cb) 
{
    m_pushCallback = cb; // Register callback
}

void CommunicationHandler::enablePushChannel() 
{
    // If channel wasnt active and connection is esabilished atm, register channel
    // If connection is not active, let openConnection() take care of it
    if(!m_isPushChannel && m_connectStatus == CONNECTED && m_pushCallback)
        registerPushChannel(m_pushCallback);

    m_isPushChannel = true;
}

void CommunicationHandler::disablePushChannel() 
{
    // If connection is not active theres no way to close PUSH channel
    if(m_isPushChannel && m_connectStatus == CONNECTED)
        closePushChannel();

    m_isPushChannel = false;
}
    
int CommunicationHandler::openConnection()
{
    boost::unique_lock<boost::mutex> lock(m_connectMutex);
    websocketpp::lib::error_code ec;
        
    if(m_connectStatus == CONNECTED)
        return 0;
    
    m_connectStatus = TIMEOUT;

    // Initialize ASIO
    if(m_endpoint) { // If endpoint exists, then make sure that previous worker thread are stopped before we destroy that io_service
        m_endpoint->stop();
        m_worker1.join();
        m_worker2.join();
    }
    
    // (re)Initialize endpoint (io_service)
    m_endpoint.reset(new ws_client());
    m_endpoint->clear_access_channels(websocketpp::log::alevel::all);
    m_endpoint->clear_error_channels(websocketpp::log::elevel::all);
    m_endpoint->init_asio(new boost::asio::io_service(), ec);
    
    if(ec)
    {
        LOG(ERROR) << "Cannot initlize WebSocket endpoint";
        return m_connectStatus;
    }
        
    // Register our handlers
    m_endpoint->set_tls_init_handler(bind(&CommunicationHandler::onTLSInit, this, ::_1));
    m_endpoint->set_socket_init_handler(bind(&CommunicationHandler::onSocketInit, this, ::_1, ::_2));    // On socket init
    m_endpoint->set_message_handler(bind(&CommunicationHandler::onMessage, this, ::_1, ::_2));           // Incoming WebSocket message
    m_endpoint->set_open_handler(bind(&CommunicationHandler::onOpen, this, ::_1));                       // WebSocket connection estabilished
    m_endpoint->set_close_handler(bind(&CommunicationHandler::onClose, this, ::_1));                     // WebSocket connection closed
    m_endpoint->set_fail_handler(bind(&CommunicationHandler::onFail, this, ::_1));
    m_endpoint->set_ping_handler(bind(&CommunicationHandler::onPing, this, ::_1, ::_2));
    m_endpoint->set_pong_handler(bind(&CommunicationHandler::onPong, this, ::_1, ::_2));
    m_endpoint->set_pong_timeout_handler(bind(&CommunicationHandler::onPongTimeout, this, ::_1, ::_2));
    m_endpoint->set_interrupt_handler(bind(&CommunicationHandler::onInterrupt, this, ::_1));
    
    string URL = string("wss://") + m_hostname + ":" + toString(m_port) + string(CLUSTER_URI_PATH);
    ws_client::connection_ptr con = m_endpoint->get_connection(URL, ec); // Initialize WebSocket handshake
    if(ec.value() != 0) {
        LOG(ERROR) << "Cannot connect to " << URL << " due to: " << ec.message();
        return ec.value();
    } else {
        LOG(INFO) << "Trying to connect to: " << URL;
    }
    
    m_endpoint->connect(con);
    m_endpointConnection = con;
    
    // Start worker thread(s)
    // Second worker should not be started if WebSocket client lib cannot handle full-duplex connections
    m_worker1 = websocketpp::lib::thread(&ws_client::run, m_endpoint);
    //m_worker2 = websocketpp::lib::thread(&ws_client::run, m_endpoint);
    
    // Wait for WebSocket handshake
    m_connectCond.timed_wait(lock, boost::posix_time::milliseconds(CONNECT_TIMEOUT));
    
    LOG(INFO) << "Connection to " << URL << " status: " << m_connectStatus;
    
    if(m_connectStatus == 0 && !sendHandshakeACK())
        LOG(WARNING) << "Cannot set fuseId for the connection. Cluster will reject most of messages.";

    if(m_connectStatus == 0 && m_isPushChannel && m_pushCallback && m_fuseID.size() > 0)
        registerPushChannel(m_pushCallback);
        
    return m_connectStatus;
}
    
    
void CommunicationHandler::closeConnection()
{
    boost::unique_lock<boost::mutex> lock(m_connectMutex);
    
    if(m_connectStatus == CLOSED)
        return;
    
    if(m_endpoint) // Do not attempt to close connection if underlying io_service wasnt initialized
    {
        websocketpp::lib::error_code ec;
        m_endpoint->close(m_endpointConnection, websocketpp::close::status::normal, string("reset_by_peer"), ec); // Initialize WebSocket cloase operation
        if(ec.value() == 0)
            m_connectCond.timed_wait(lock, boost::posix_time::milliseconds(CONNECT_TIMEOUT)); // Wait for connection to close
        
        m_endpoint->stop(); // If connection failed to close, make sure that io_service refuses to send/receive any further messages at this point
        
        try {
            LOG(INFO) << "WebSocket: Lowest layer socket closed.";
            m_endpointConnection->get_socket().lowest_layer().close();  // Explicite close underlying socket to make sure that all ongoing operations will be canceld
        } catch (boost::exception &e) {
            LOG(ERROR) << "WebSocket connection socket close error";
        }
    }
    
    // Stop workers
    m_worker1.join();
    m_worker2.join();
    
    m_connectStatus = CLOSED;
}
    
    
void CommunicationHandler::registerPushChannel(push_callback callback)
{
    LOG(INFO) << "Sending registerPushChannel request with FuseId: " << m_fuseID;

    m_pushCallback = callback; // Register callback
    m_isPushChannel = true;
    
    // Prepare PUSH channel registration request message
    ClusterMsg msg;
    ChannelRegistration reg;
    Atom at;
    reg.set_fuse_id(m_fuseID);
    
    msg.set_module_name("fslogic");
    msg.set_protocol_version(PROTOCOL_VERSION);
    msg.set_message_type(helpers::utils::tolower(reg.GetDescriptor()->name()));
    msg.set_message_decoder_name(helpers::utils::tolower(FUSE_MESSAGES));
    msg.set_answer_type(helpers::utils::tolower(Atom::descriptor()->name()));
    msg.set_answer_decoder_name(helpers::utils::tolower(COMMUNICATION_PROTOCOL));
    msg.set_synch(1);
    msg.set_input(reg.SerializeAsString());
    
    Answer ans = communicate(msg, 0);    // Send PUSH channel registration request
    at.ParseFromString(ans.worker_answer());
    
    LOG(INFO) << "PUSH channel registration status: " << ans.worker_answer() << ": " << at.value(); 
}

bool CommunicationHandler::sendHandshakeACK() 
{
    ClusterMsg msg;
    HandshakeAck ack;

    LOG(INFO) << "Sending HandshakeAck with fuseId: '" << m_fuseID << "'";

    // Build HandshakeAck message
    ack.set_fuse_id(m_fuseID);

    msg.set_module_name("");
    msg.set_protocol_version(PROTOCOL_VERSION);
    msg.set_message_type(helpers::utils::tolower(ack.GetDescriptor()->name()));
    msg.set_message_decoder_name(helpers::utils::tolower(FUSE_MESSAGES));
    msg.set_answer_type(helpers::utils::tolower(Atom::descriptor()->name()));
    msg.set_answer_decoder_name(helpers::utils::tolower(COMMUNICATION_PROTOCOL));
    msg.set_synch(1);
    msg.set_input(ack.SerializeAsString());

    // Send HandshakeAck to cluster
    Answer ans = communicate(msg, 0);

    return ans.answer_status() == VOK;
}
    
void CommunicationHandler::closePushChannel()
{
    m_isPushChannel = false;
    
    // Prepare PUSH channel unregistration request message
    ClusterMsg msg;
    ChannelClose reg;
    reg.set_fuse_id(m_fuseID);
    
    msg.set_module_name("fslogic");
    msg.set_protocol_version(PROTOCOL_VERSION);
    msg.set_message_type(helpers::utils::tolower(reg.GetDescriptor()->name()));
    msg.set_message_decoder_name(helpers::utils::tolower(FUSE_MESSAGES));
    msg.set_answer_type(helpers::utils::tolower(Atom::descriptor()->name()));
    msg.set_answer_decoder_name(helpers::utils::tolower(COMMUNICATION_PROTOCOL));
    msg.set_synch(1);
    msg.set_input(reg.SerializeAsString());
    
    Answer ans = communicate(msg, 0);    // Send PUSH channel close request
}

int CommunicationHandler::sendMessage(const ClusterMsg& msg, int32_t msgId)
{
    if(m_connectStatus != CONNECTED)
        return m_connectStatus;
    
    ClusterMsg msgWithId = msg;
    msgWithId.set_message_id(msgId);
    
    websocketpp::lib::error_code ec;
    m_endpoint->send(m_endpointConnection, msgWithId.SerializeAsString(), websocketpp::frame::opcode::binary, ec); // Initialize send operation (async)
    
    if(ec.value())
        ++m_errorCount;
    
    return ec.value();
}
    
int32_t CommunicationHandler::getMsgId()
{
    boost::unique_lock<boost::mutex> lock(m_msgIdMutex);
    ++m_currentMsgId;
    if(m_currentMsgId <= 0) // Skip 0 and negative values
        m_currentMsgId = 1;
    
    return m_currentMsgId;
}
    
int CommunicationHandler::receiveMessage(Answer& answer, int32_t msgId, uint32_t timeout)
{
    boost::unique_lock<boost::mutex> lock(m_receiveMutex);
    
    // Incoming message should be in inbox. Wait for it
    while(m_incomingMessages.find(msgId) == m_incomingMessages.end())
        if(!m_receiveCond.timed_wait(lock, boost::posix_time::milliseconds(timeout))) {
            ++m_errorCount;
            return -1;
        }
    
    answer.ParseFromString(m_incomingMessages[msgId]);
    m_incomingMessages.erase(m_incomingMessages.find(msgId));
        
    return 0;
}
    
Answer CommunicationHandler::communicate(ClusterMsg& msg, uint8_t retry, uint32_t timeout)
{
    Answer answer;
    try
    {
        unsigned int msgId = getMsgId();
        
        if(sendMessage(msg, msgId) != 0)
        {
            if(retry > 0) 
            {
                LOG(WARNING) << "Sening message to cluster failed, trying to reconnect and retry";
                closeConnection();
                if(openConnection() == 0)
                    return communicate(msg, retry - 1);
            }
                
            LOG(ERROR) << "WebSocket communication error";
            answer.set_answer_status(VEIO);

            return answer;
        }

        if(receiveMessage(answer, msgId, timeout) != 0)
        {
            if(retry > 0) 
            {
                LOG(WARNING) << "Receiving response from cluster failed, trying to reconnect and retry";
                closeConnection();
                if(openConnection() == 0)
                    return communicate(msg, retry - 1);
            }

            LOG(ERROR) << "WebSocket communication error";
            answer.set_answer_status(VEIO);
        }

        if(answer.answer_status() != VOK) 
        {
            LOG(INFO) << "Received answer with non-ok status: " << answer.answer_status();
            
            // Dispatch error to client if possible using PUSH channel 
            if(m_pushCallback)
                m_pushCallback(answer);
        }

        
    } catch (websocketpp::lib::error_code &e) {
        LOG(ERROR) << "Unhandled WebSocket exception: " << e.message();
    }
    
    return answer;
}

int CommunicationHandler::getInstancesCount()
{
    boost::unique_lock<boost::mutex> lock(m_instanceMutex);
    return instancesCount;
}
    
context_ptr CommunicationHandler::onTLSInit(websocketpp::connection_hdl hdl)
{
    // Setup TLS connection (i.e. certificates)
    context_ptr ctx(new boost::asio::ssl::context(boost::asio::ssl::context::sslv3));
        
    try {
        ctx->set_options(boost::asio::ssl::context::default_workarounds |
                         boost::asio::ssl::context::no_sslv2 |
                         boost::asio::ssl::context::single_dh_use);
            
        ctx->use_certificate_chain_file(m_certPath);
        ctx->use_private_key_file(m_certPath, boost::asio::ssl::context::pem);
    } catch (std::exception& e) {
        LOG(ERROR) << "Cannot initialize TLS socket due to:" << e.what();
    }
        
    return ctx;
}
    
void CommunicationHandler::onSocketInit(websocketpp::connection_hdl hdl,socket_type &socket)
{
    // Disable socket delay
    socket.lowest_layer().set_option(boost::asio::ip::tcp::no_delay(true));
}
    
void CommunicationHandler::onMessage(websocketpp::connection_hdl hdl, message_ptr msg)
{
    Answer answer;
    
    if(!answer.ParseFromString(msg->get_payload()))   // Ignore invalid answer
        return;
    
    if(answer.message_id() < 0) // PUSH message
    {
        if(m_pushCallback)
            m_pushCallback(answer); // Dispatch PUSH message to registered callback
        else
            LOG(WARNING) << "Received PUSH message (ID: " << answer.message_id() <<") but the channel is not registered as PUSH listener. Ignoring.";
            
        return;
    }
    
    // Continue normally for non-PUSH message
    boost::unique_lock<boost::mutex> lock(m_receiveMutex);
    m_incomingMessages[answer.message_id()] = msg->get_payload();         // Save incloming message to inbox and notify waiting threads
    m_receiveCond.notify_all();
}
    
void CommunicationHandler::onOpen(websocketpp::connection_hdl hdl)
{
    boost::unique_lock<boost::mutex> lock(m_connectMutex);
    m_connectStatus = CONNECTED;
    LOG(INFO) << "WebSocket connection esabilished successfully.";
    m_connectCond.notify_all();
}
    
void CommunicationHandler::onClose(websocketpp::connection_hdl hdl)
{
    boost::unique_lock<boost::mutex> lock(m_connectMutex);
    ++m_errorCount; // Closing connection means that something went wrong

    m_connectStatus = CLOSED;
    m_connectCond.notify_all();
}
    
void CommunicationHandler::onFail(websocketpp::connection_hdl hdl)
{
    boost::unique_lock<boost::mutex> lock(m_connectMutex);
    m_connectStatus = HANDSHAKE_ERROR;
    m_connectCond.notify_all();
    
    LOG(ERROR) << "WebSocket handshake error";
}
    
bool CommunicationHandler::onPing(websocketpp::connection_hdl hdl, std::string msg)
{
    // No need to implement this
    return true;
}
    
void CommunicationHandler::onPong(websocketpp::connection_hdl hdl, std::string msg)
{
    // Since we dont ping cluster, this callback wont be called
}
    
void CommunicationHandler::onPongTimeout(websocketpp::connection_hdl hdl, std::string msg)
{
    // Since we dont ping cluster, this callback wont be called
    LOG(WARNING) << "WebSocket pong-message (" << msg << ") timed out";
}
    
void CommunicationHandler::onInterrupt(websocketpp::connection_hdl hdl)
{
    LOG(WARNING) << "WebSocket connection was interrupted";
    boost::unique_lock<boost::mutex> lock(m_connectMutex);
    m_connectStatus = TRANSPORT_ERROR;
}

} // namespace veil