/**
 * @file pong.cc
 * @author Krzysztof Trzepla
 * @copyright (C) 2015 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "messages/pong.h"

#include "server_messages.pb.h"

namespace one {
namespace messages {

Pong::Pong(std::unique_ptr<ProtocolServerMessage> serverMessage) {}

} // namespace messages
} // namespace one