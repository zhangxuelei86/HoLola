#pragma once
#ifndef _VISION_LISTENER_H
#define _VISION_LISTENER_H
///////////////////////////////////////////
//
// Provides convenient access to vision
// messages from lepp3 (or other sources)
//
//////////////////////////////////////////

#include <functional>
#include <thread>

#include <iface_vision_msg.hpp>
#include <iface_msg.hpp>

#include "sock_utils.h"
#include "Logging.h"

class VisionListener
{
public:
    typedef std::function<void(am2b_iface::VisionMessage*)>     OnVisionMessage;
    typedef std::function<void(am2b_iface::ObstacleMessage*)>   OnObstacleMessage;
    typedef std::function<void(am2b_iface::SurfaceMessage*)>    OnSurfaceMessage;
    typedef std::function<void(am2b_iface::RGBMessage*)>        OnRGBMessage;
    typedef std::function<void(am2b_iface::PointCloudMessage*)> OnPointCloudMessage;
    typedef std::function<void(std::wstring)>                   OnNewConnection;
    typedef std::function<void(std::wstring)>                   OnDisconnect;
    typedef std::function<void(std::wstring)>                   OnError;

    VisionListener(int port) : _port(port) { LogInfo(L"New VisionListener created"); }
    ~VisionListener()
    {
        LogInfo(L"VisionListener destroyed!");
        stop();
    }

    void listen()
    {
        if (_listening)
            return;

        _listener = std::thread(&VisionListener::listen_impl, this);
    }

    bool listening()
    {
        return _listening;
    }

    void stop()
    {
        if (_listening)
        {
            _listening = false;
            if (_listener.joinable())
                _listener.join();
        }
    }

    // callback connections
    void onConnect(OnNewConnection cb)               { _onConnect           = cb; }
    void onDisconnect(OnDisconnect cb)               { _onDisconnect        = cb; }
    void onError(OnError cb)		                 { _onError             = cb; }
    void onVisionMessage(OnVisionMessage cb)         { _onVisionMessage     = cb; }
    void onObstacleMessage(OnObstacleMessage cb)     { _onObstacleMessage   = cb; }
    void onSurfaceMessage(OnSurfaceMessage cb)       { _onSurfaceMessage    = cb; }
    void onRGBMessage(OnRGBMessage cb)               { _onRGBMessage        = cb; }
    void onPointCloudMessage(OnPointCloudMessage cb) { _onPointCloudMessage = cb; }

private:
    bool _verbose = true;
    int  _port;
    int  _socket;
    bool _listening = false;
    std::thread _listener;
    size_t _buflen = 2048;

    OnError             _onError;
    OnNewConnection     _onConnect;
    OnDisconnect        _onDisconnect;
    OnVisionMessage     _onVisionMessage;
    OnSurfaceMessage    _onSurfaceMessage;
    OnObstacleMessage   _onObstacleMessage;
    OnRGBMessage        _onRGBMessage;
    OnPointCloudMessage _onPointCloudMessage;

    // wrapper to ensure safe use of callbacks
    template <class Functor, class... Args>
    void cb(Functor&& f, Args&&... args)
    {
        if (f)
            f(std::forward<Args>(args)...);
    }

    void listen_impl()
    {
        struct sockaddr_in si_other;
        int s_other;
        socklen_t slen = sizeof(si_other);
        struct timeval timeout; // timeout for select
        _socket = create_server_socket(_port);

        if (_socket == INVALID_SOCKET)
        {
            EventWriteVision_OnConnectionError(std::to_wstring(_port).c_str());
            cb(_onError, L"[Vision] Could not create socket on port: " + std::to_wstring(_port));
            return;
        }

        // set socket to non-blocking
        unsigned long on = 1;
        ioctlsocket(_socket, FIONBIO, &on);
        _listening = true;

        while (_listening)
        {
            timeout.tv_sec = 0;
            timeout.tv_usec = 500000; // 0.5 seconds
            fd_set readfds; FD_ZERO(&readfds);
            int maxfd, fd;

            maxfd = -1;
            FD_SET(_socket, &readfds);
            maxfd = _socket;

            if (select(maxfd + 1, &readfds, NULL, NULL, &timeout) <= 0)
                continue;
            fd = -1;

            if (FD_ISSET(_socket, &readfds))
            {
                fd = _socket;
            }

            if (fd == -1)
            {
                cb(_onError, GetWSAErrorStr(WSAGetLastError(), L"Invalid socket returned from select!"));
                EventWriteVision_OnConnectionError(std::to_wstring(_port).c_str());
            }

            s_other = accept(fd, (struct sockaddr*) &si_other, &slen);
            if (s_other < 0)
            {
                cb(_onError, GetWSAErrorStr(WSAGetLastError(), L"accept failed!"));
                EventWriteVision_OnConnectionError(std::to_wstring(_port).c_str());
            }

            cb(_onConnect, hostString(si_other));
            EventWriteVision_OnConnectionOpened(hostString(si_other).c_str());
            if (fd == _socket)
            {
                std::thread servicer(&VisionListener::readVisionMessagesFrom, this, s_other, si_other);
                servicer.detach();
            }
        }
    }

    void readVisionMessagesFrom(int socket_remote, const sockaddr_in& si_other)
    {
        std::vector<char> buf;
        buf.resize(_buflen);

        while (_listening)
        {
            size_t ifaceHeaderSize = sizeof(am2b_iface::MsgHeader);
            size_t visionHeaderSize = sizeof(am2b_iface::VisionMessageHeader);
            size_t total_received = 0;
            size_t total_expected = ifaceHeaderSize;

            if (_verbose)
            {
                LogInfo(L"Waiting for am2b_iface::MsgHeader (" + std::to_wstring(ifaceHeaderSize) + L" bytes)...");
            }

            while (total_received < total_expected)
            {
                int recvd = 0;
                recvd = recv(socket_remote, &buf[total_received], total_expected - total_received, 0);

                if (recvd == 0) // connection died
                {
                    break;
                }
                if (recvd == SOCKET_ERROR) // failed to read from socket
                {
                    auto err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK) // keep waiting
                    {
                        continue;
                    }
                    else
                    {
                        cb(_onError, GetWSAErrorStr(err, L"read() failed!"));
                        break;
                    }
                }

                total_received += recvd;

                if (_verbose)
                {
                    LogInfo(std::wstring(L"(iface header) Received ") + std::to_wstring(total_received).c_str() + L" / " + std::to_wstring(total_expected) + L" total bytes from " + hostString(si_other).c_str());
                }
            }

            if (total_received < ifaceHeaderSize) // hit an error above
                break;

            am2b_iface::MsgHeader* iface_header = (am2b_iface::MsgHeader*)buf.data();
            if (_verbose)
            {
                LogInfo(std::wstring(L"Received am2b_iface::MsgHeader: id = ") + std::to_wstring(iface_header->id) + L", len = " + std::to_wstring(iface_header->len));
            }

            total_expected += iface_header->len;
            if (buf.size() < total_expected)
            {
                if (_verbose)
                {
                    LogInfo(std::wstring(L"Resizing receive buffer to ") + std::to_wstring(total_expected) + L" bytes");
                }
                buf.resize(total_expected);
                iface_header = (am2b_iface::MsgHeader*)buf.data(); // update pointer after re-allocation
            }

            while (total_received < total_expected)
            {
                int recvd = 0;
                recvd = recv(socket_remote, &buf[total_received], total_expected - total_received, 0);

                if (recvd == 0) // connection died
                    break;
                if (recvd == SOCKET_ERROR)
                {
                    auto err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK) // should keep waiting
                    {
                        continue;
                    }
                    else
                    {
                        cb(_onError, GetWSAErrorStr(err, L"read() failed!"));
                        break;
                    }
                }

                total_received += recvd;
                if (_verbose)
                {
                    LogInfo(std::wstring(L"(message) Received ") + std::to_wstring(total_received) + L" / " + std::to_wstring(total_expected) + L" total bytes from " + hostString(si_other));
                }
            }

            if (total_received < total_expected) // hit an error above
                break;

            if (total_expected != total_received)
            {
                LogInfo(L"TOTAL RECVD: " + std::to_wstring(total_received) + L" : EXPECTED: " + std::to_wstring(sizeof(am2b_iface::MsgHeader)) + std::to_wstring(iface_header->len));
                break;
            }
            else
            {
                total_received = 0;
            }

            if (iface_header->id != am2b_iface::VISION_MESSAGE)
            {
                std::cout << "Received non-vision message type '0x"
                    << std::hex << iface_header->id << std::dec
                    << "' on vision port. Discarding it..." << std::endl;
                continue;
            }

            am2b_iface::VisionMessageHeader* visionHeader = (am2b_iface::VisionMessageHeader*)(buf.data() + sizeof(am2b_iface::MsgHeader));
            if (_verbose)
            {
                std::cout << "Received VisionMessageHeader: " << *visionHeader << std::endl;
                std::cout << "Should have " << visionHeader->len + sizeof(am2b_iface::VisionMessageHeader)
                    << " bytes (got " << total_received << ")" << std::endl;
            }
            EventWriteVision_OnVisionMessageReceived(L"Received VisionMessage");

            switch (visionHeader->type)
            {
                case am2b_iface::Message_Type::Obstacle:
                {
                    am2b_iface::ObstacleMessage* o = (am2b_iface::ObstacleMessage*)(buf.data() + sizeof(am2b_iface::VisionMessageHeader) + sizeof(am2b_iface::MsgHeader));
                    EventWriteVision_OnObstacleMessageReceived(o->type, o->model_id, o->part_id, o->action, o->radius, o->surface, o->coeffs);
                    cb(_onObstacleMessage, o);
                    break;
                }
                case am2b_iface::Message_Type::Surface:
                {
                    EventWriteVision_OnSurfaceMessageReceived(L"New Surface Received");
                    cb(_onSurfaceMessage,
                        (am2b_iface::SurfaceMessage*)
                        (buf.data() + sizeof(am2b_iface::VisionMessageHeader) + sizeof(am2b_iface::MsgHeader)));
                    break;
                }
                case am2b_iface::Message_Type::RGB_Image:
                {
                    EventWriteVision_OnRGBImageMessageReceived(L"New RGB Image Received");
                    am2b_iface::RGBMessage* message = (am2b_iface::RGBMessage*)
                        (buf.data()
                            + sizeof(am2b_iface::VisionMessageHeader) + sizeof(am2b_iface::MsgHeader));
                    message->pixels = (unsigned char*)((char*)message + sizeof(am2b_iface::RGBMessage)); // fix pointer
                    cb(_onRGBMessage, message);
                    break;
                }
                case am2b_iface::Message_Type::PointCloud:
                {
                    EventWriteVision_OnPointCloudMessageReceived(L"New PointCloud Received");
                    am2b_iface::PointCloudMessage* message = (am2b_iface::PointCloudMessage*)
                        (buf.data()
                            + sizeof(am2b_iface::VisionMessageHeader) + sizeof(am2b_iface::MsgHeader));
                    message->data = (unsigned char*)((char*)message + sizeof(am2b_iface::PointCloudMessage));
                    cb(_onPointCloudMessage, message);
                    break;
                }
                default:
                {
                    EventWriteVision_OnUnknownVisionMessageReceived(L"BAD Vision Message received!");
                    std::cout << "UNKNOWN message type: " << visionHeader->type << "!!" << std::endl;
                }
            }
        }

        closesocket(socket_remote);
        cb(_onDisconnect, hostString(si_other));
        EventWriteVision_OnConnectionClosed(hostString(si_other).c_str());
    }

};

#endif // _VISION_LISTENER_H
