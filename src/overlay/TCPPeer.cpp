// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TCPPeer.h"
#include "util/Logging.h"
#include "main/Application.h"
#include "overlay/StellarXDR.h"
#include "xdrpp/marshal.h"
#include "overlay/OverlayManager.h"
#include "database/Database.h"
#include "overlay/PeerRecord.h"
#include "medida/metrics_registry.h"
#include "medida/meter.h"
#include "main/Config.h"

#define IO_TIMEOUT_SECONDS 30
#define MAX_MESSAGE_SIZE 0x1000000

using namespace soci;

namespace stellar
{

using namespace std;

///////////////////////////////////////////////////////////////////////
// TCPPeer
///////////////////////////////////////////////////////////////////////

TCPPeer::TCPPeer(Application& app, Peer::PeerRole role,
                 std::shared_ptr<asio::ip::tcp::socket> socket)
    : Peer(app, role)
    , mSocket(socket)
    , mIdleTimer(app)
    , mLastRead(app.getClock().now())
    , mLastWrite(app.getClock().now())
    , mStrand(app.getClock().getIOService())
    , mMessageRead(
          app.getMetrics().NewMeter({"overlay", "message", "read"}, "message"))
    , mMessageWrite(
          app.getMetrics().NewMeter({"overlay", "message", "write"}, "message"))
    , mByteRead(app.getMetrics().NewMeter({"overlay", "byte", "read"}, "byte"))
    , mByteWrite(
          app.getMetrics().NewMeter({"overlay", "byte", "write"}, "byte"))
    , mErrorRead(
          app.getMetrics().NewMeter({"overlay", "error", "read"}, "error"))
    , mErrorWrite(
          app.getMetrics().NewMeter({"overlay", "error", "write"}, "error"))
    , mTimeoutRead(
          app.getMetrics().NewMeter({"overlay", "timeout", "read"}, "timeout"))
    , mTimeoutWrite(
          app.getMetrics().NewMeter({"overlay", "timeout", "write"}, "timeout"))
{
}

TCPPeer::pointer
TCPPeer::initiate(Application& app, std::string const& ip, unsigned short port)
{
    CLOG(DEBUG, "Overlay") << "TCPPeer:initiate"
                           << " to " << ip << ":" << port;
    auto socket =
        make_shared<asio::ip::tcp::socket>(app.getClock().getIOService());
    auto result = make_shared<TCPPeer>(app, WE_CALLED_REMOTE, socket);
    result->mIP = ip;
    result->mRemoteListeningPort = port;
    asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string(ip), port);
    socket->async_connect(
        endpoint, result->mStrand.wrap([result](asio::error_code const& error)
                                       {
                                           result->connectHandler(error);
                                       }));
    return result;
}

TCPPeer::pointer
TCPPeer::accept(Application& app, shared_ptr<asio::ip::tcp::socket> socket)
{
    CLOG(DEBUG, "Overlay") << "TCPPeer:accept"
                           << "@" << app.getConfig().PEER_PORT;
    auto result = make_shared<TCPPeer>(app, REMOTE_CALLED_US, socket);
    result->mIP = socket->remote_endpoint().address().to_string();
    result->startRead();
    return result;
}

TCPPeer::~TCPPeer()
{
    mIdleTimer.cancel();
    try
    {
        if (mSocket)
        {
#ifndef _WIN32
            // This always fails on windows and ASIO won't
            // even build it.
            mSocket->cancel();
#endif
            mSocket->close();
        }
    }
    catch (asio::system_error&)
    {
        // Ignore: this indicates an attempt to cancel events
        // on a not-established socket.
    }
}

void
TCPPeer::startIdleTimer()
{
    if (shouldAbort())
    {
        return;
    }

    std::shared_ptr<TCPPeer> self =
        std::static_pointer_cast<TCPPeer>(shared_from_this());
    mIdleTimer.expires_from_now(std::chrono::seconds(IO_TIMEOUT_SECONDS));
    mIdleTimer.async_wait(mStrand.wrap([self](asio::error_code const& error)
                                       {
                                           self->idleTimerExpired(error);
                                       }));
}

void
TCPPeer::idleTimerExpired(asio::error_code const& error)
{
    if (!error)
    {
        auto now = mApp.getClock().now();
        if (now - mLastRead > std::chrono::seconds(IO_TIMEOUT_SECONDS))
        {
            CLOG(WARNING, "Overlay") << "read timeout";
            mTimeoutRead.Mark();
            drop();
        }
        else if (now - mLastWrite > std::chrono::seconds(IO_TIMEOUT_SECONDS))
        {
            CLOG(WARNING, "Overlay") << "write timeout";
            mTimeoutWrite.Mark();
            drop();
        }
        else
        {
            startIdleTimer();
        }
    }
}

std::string
TCPPeer::getIP()
{
    return mIP;
}

void
TCPPeer::sendMessage(xdr::msg_ptr&& xdrBytes)
{
    CLOG(TRACE, "Overlay") << "TCPPeer:sendMessage to " << toString();

    bool wasEmpty = mWriteQueue.empty();

    // places the buffer to write into the write queue
    auto buf = std::make_shared<xdr::msg_ptr>(std::move(xdrBytes));
    mWriteQueue.emplace(buf);

    if (wasEmpty)
    {
        // kick off the async write chain if we're the first one
        messageSender();
    }
}

void
TCPPeer::messageSender()
{
    // if nothing to do, return
    if (mWriteQueue.empty())
    {
        return;
    }

    mLastWrite = mApp.getClock().now();

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    // peek the buffer from the queue
    // do not remove it yet as we need the buffer for the duration of the
    // write operation
    auto buf = mWriteQueue.front();

    asio::async_write(
        *(mSocket.get()), asio::buffer((*buf)->raw_data(), (*buf)->raw_size()),
        mStrand.wrap([self](asio::error_code const& ec, std::size_t length)
                     {
                         self->writeHandler(ec, length);
                         self->mWriteQueue.pop(); // done with front element
                         self->messageSender();   // send the next one
                     }));
}

void
TCPPeer::writeHandler(asio::error_code const& error,
                      std::size_t bytes_transferred)
{
    if (error)
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            mErrorWrite.Mark();
            CLOG(ERROR, "Overlay") << "TCPPeer::writeHandler error to "
                                   << toString();
        }
        drop();
    }
    else
    {
        mMessageWrite.Mark();
        mByteWrite.Mark(bytes_transferred);
    }
}

void
TCPPeer::startRead()
{
    if (shouldAbort())
    {
        return;
    }

    try
    {
        auto self = static_pointer_cast<TCPPeer>(shared_from_this());

        assert(self->mIncomingHeader.size() == 0);
        CLOG(TRACE, "Overlay") << "TCPPeer::startRead to "
                               << self->toString();

        self->mLastRead = self->mApp.getClock().now();
        self->mIncomingHeader.resize(4);
        asio::async_read(*(self->mSocket.get()),
                         asio::buffer(self->mIncomingHeader),
                         self->mStrand.wrap(
                             [self](asio::error_code ec, std::size_t length)
                             {
                                 CLOG(TRACE, "Overlay")
                                     << "TCPPeer::startRead calledback "
                                     << ec << " length:" << length;
                                 self->readHeaderHandler(ec, length);
                             }));
    }
    catch (asio::system_error& e)
    {
        CLOG(ERROR, "Overlay") << "TCPPeer::startRead error " << e.what();
        drop();
    }
}

int
TCPPeer::getIncomingMsgLength()
{
    int length = mIncomingHeader[0];
    length &= 0x7f; // clear the XDR 'continuation' bit
    length <<= 8;
    length |= mIncomingHeader[1];
    length <<= 8;
    length |= mIncomingHeader[2];
    length <<= 8;
    length |= mIncomingHeader[3];
    if (length < 0 || length > MAX_MESSAGE_SIZE)
    {
        mErrorRead.Mark();
        CLOG(ERROR, "Overlay")
            << "TCP::Peer::getIncomingMsgLength message size unacceptable: "
            << length;
        drop();
    }
    return (length);
}

void
TCPPeer::connected()
{
    startRead();
}

void
TCPPeer::readHeaderHandler(asio::error_code const& error,
                           std::size_t bytes_transferred)
{
    // LOG(DEBUG) << "TCPPeer::readHeaderHandler "
    //     << "@" << mApp.getConfig().PEER_PORT
    //     << " to " << mRemoteListeningPort
    //     << (error ? "error " : "") << " bytes:" << bytes_transferred;

    if (!error)
    {
        mByteRead.Mark(bytes_transferred);
        mIncomingBody.resize(getIncomingMsgLength());
        auto self = static_pointer_cast<TCPPeer>(shared_from_this());
        asio::async_read(
            *mSocket.get(), asio::buffer(mIncomingBody),
            self->mStrand.wrap([self](asio::error_code ec, std::size_t length)
                               {
                                   self->mIncomingHeader.clear();
                                   self->readBodyHandler(ec, length);
                               }));
    }
    else
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            mErrorRead.Mark();
            CLOG(DEBUG, "Overlay")
                << "readHeaderHandler error: " << error.message() << " :"
                << toString();
        }
        drop();
    }
}

void
TCPPeer::readBodyHandler(asio::error_code const& error,
                         std::size_t bytes_transferred)
{
    // LOG(DEBUG) << "TCPPeer::readBodyHandler "
    //     << "@" << mApp.getConfig().PEER_PORT
    //     << " to " << mRemoteListeningPort
    //     << (error ? "error " : "") << " bytes:" << bytes_transferred;

    if (!error)
    {
        mByteRead.Mark(bytes_transferred);
        recvMessage();
        startRead();
    }
    else
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            mErrorRead.Mark();
            CLOG(ERROR, "Overlay")
                << "readBodyHandler error: " << error.message() << " :"
                << toString();
        }
        drop();
    }
}

void
TCPPeer::recvMessage()
{
    try
    {
        xdr::xdr_get g(mIncomingBody.data(),
                       mIncomingBody.data() + mIncomingBody.size());
        mMessageRead.Mark();
        if (isAuthenticated())
        {
            AuthenticatedMessage am;
            xdr::xdr_argpack_archive(g, am);
            Peer::recvMessage(am);
        }
        else
        {
            StellarMessage sm;
            xdr::xdr_argpack_archive(g, sm);
            Peer::recvMessage(sm);
        }
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG(ERROR, "Overlay") << "recvMessage got a corrupt xdr: " << e.what();
        drop();
    }
}

void
TCPPeer::drop()
{
    if (mState == CLOSING)
    {
        return;
    }

    CLOG(DEBUG, "Overlay") << "TCPPeer::drop " << toString() << " in state "
                           << mState << " we called:" << mRole;

    mState = CLOSING;

    mIdleTimer.cancel();

    auto self = shared_from_this();
    getApp().getOverlayManager().dropPeer(self);

    // close connection, abort all transmissions immediately
    // this causes all read/write callbacks to be invoked with an error
    try
    {
        mSocket->close();
    }
    catch (asio::system_error& e)
    {
        CLOG(ERROR, "Overlay")
            << "TCPPeer::drop close socket failed: " << e.what();
    }
    catch (...)
    {
        CLOG(ERROR, "Overlay") << "TCPPeer::drop close socket failed";
    }
}
}
