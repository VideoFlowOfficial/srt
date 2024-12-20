#include <array>
#include <future>
#include <thread>
#include <chrono>
#include <vector>
#include <functional>

#include "gtest/gtest.h"
#include "test_env.h"

#include "srt.h"
#include "netinet_any.h"
#include "socketconfig.h"

TEST(Bonding, SRTConnectGroup)
{
    srt::TestInit srtinit;
    struct sockaddr_in sa;

    const int ss = srt_create_group(SRT_GTYPE_BROADCAST);
    ASSERT_NE(ss, SRT_ERROR);

    std::vector<SRT_SOCKGROUPCONFIG> targets;
    for (int i = 0; i < 2; ++i)
    {
        sa.sin_family = AF_INET;
        sa.sin_port = htons(4200 + i);
        ASSERT_EQ(inet_pton(AF_INET, "192.168.1.237", &sa.sin_addr), 1);

        const SRT_SOCKGROUPCONFIG gd = srt_prepare_endpoint(NULL, (struct sockaddr*)&sa, sizeof sa);
        targets.push_back(gd);
    }

    std::future<void> closing_promise = std::async(std::launch::async, [](int s) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::cerr << "Closing group" << std::endl;
        srt_close(s);
    }, ss);

    std::cout << "srt_connect_group calling " << std::endl;
    const int st = srt_connect_group(ss, targets.data(), (int) targets.size());
    std::cout << "srt_connect_group returned " << st << ", waiting for srt_close() to finish" << std::endl;

    closing_promise.wait();

    std::cout << "TEST: closing future has exit. Deleting all other resources\n";

    // Delete config objects before prospective exception
    for (auto& gd: targets)
        srt_delete_config(gd.config);

    int res = srt_close(ss);

    std::cout << "TEST: closing ss has exit. Cleaning up\n";
    if (res == SRT_ERROR)
    {
        std::cerr << "srt_close: " << srt_getlasterror_str() << std::endl;
    }
}

#define ASSERT_SRT_SUCCESS(callform) ASSERT_NE(callform, -1) << "SRT ERROR: " << srt_getlasterror_str()

void listening_thread(bool should_read)
{
    const SRTSOCKET server_sock = srt_create_socket();
    sockaddr_in bind_sa;
    memset(&bind_sa, 0, sizeof bind_sa);
    bind_sa.sin_family = AF_INET;
    ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &bind_sa.sin_addr), 1);
    bind_sa.sin_port = htons(4200);

    ASSERT_SRT_SUCCESS(srt_bind(server_sock, (sockaddr*)&bind_sa, sizeof bind_sa));
    const int yes = 1;
    ASSERT_SRT_SUCCESS(srt_setsockflag(server_sock, SRTO_GROUPCONNECT, &yes, sizeof yes));

    const int no = 1;
    ASSERT_SRT_SUCCESS(srt_setsockflag(server_sock, SRTO_RCVSYN, &no, sizeof no));

    const int eid = srt_epoll_create();
    const int listen_event = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    ASSERT_SRT_SUCCESS(srt_epoll_add_usock(eid, server_sock, &listen_event));

    ASSERT_SRT_SUCCESS(srt_listen(server_sock, 5));
    std::cout << "Listen: wait for acceptability\n";
    int fds[2];
    int fds_len = 2;
    int ers[2];
    int ers_len = 2;
    ASSERT_SRT_SUCCESS(srt_epoll_wait(eid, fds, &fds_len, ers, &ers_len, 5000,
            0, 0, 0, 0));

    std::cout << "Listen: reported " << fds_len << " acceptable and " << ers_len << " errors\n";
    ASSERT_GT(fds_len, 0);
    ASSERT_EQ(fds[0], server_sock);

    srt::sockaddr_any scl;
    int acp = srt_accept(server_sock, (scl.get()), (&scl.len));
    ASSERT_SRT_SUCCESS(acp);
    ASSERT_NE(acp & SRTGROUP_MASK, 0);

    if (should_read)
    {
        std::cout << "Listener will read packets...\n";
        // Read everything until closed
        int n = 0;
        for (;;)
        {
            char buf[1500];
            int rd = srt_recv(acp, buf, 1500);
            if (rd == -1)
            {
                std::cout << "Listener read " << n << " packets, stopping\n";
                break;
            }
            ++n;
        }
    }

    srt_close(acp);
    srt_close(server_sock);

    std::cout << "Listen: wait 7 seconds\n";
    std::this_thread::sleep_for(std::chrono::seconds(7));
    // srt_accept..
}

void ConnectCallback(void* /*opaq*/, SRTSOCKET sock, int error, const sockaddr* /*peer*/, int token)
{
    std::cout << "Connect callback. Socket: " << sock
        << ", error: " << error
        << ", token: " << token << '\n';
}

TEST(Bonding, NonBlockingGroupConnect)
{
    srt::TestInit srtinit;

    const int ss = srt_create_group(SRT_GTYPE_BROADCAST);
    ASSERT_NE(ss, SRT_ERROR);
    std::cout << "Created group socket: " << ss << '\n';

    int no = 0;
    ASSERT_NE(srt_setsockopt(ss, 0, SRTO_RCVSYN, &no, sizeof no), SRT_ERROR); // non-blocking mode
    ASSERT_NE(srt_setsockopt(ss, 0, SRTO_SNDSYN, &no, sizeof no), SRT_ERROR); // non-blocking mode

    const int poll_id = srt_epoll_create();
    // Will use this epoll to wait for srt_accept(...)
    const int epoll_out = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    ASSERT_NE(srt_epoll_add_usock(poll_id, ss, &epoll_out), SRT_ERROR);

    srt_connect_callback(ss, &ConnectCallback, this);

    sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(4200);
    ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

    sockaddr_in safail = sa;
    safail.sin_port = htons(4201); // port where we have no listener

    std::future<void> listen_promise = std::async(std::launch::async, std::bind(&listening_thread, false));
    
    std::cout << "Connecting two sockets " << std::endl;
    {
        const int sockid = srt_connect(ss, (sockaddr*) &sa, sizeof sa);
        EXPECT_GT(sockid, 0) << "Socket " << 1;
        sa.sin_port = htons(4201); // Changing port so that second connect fails
        std::cout << "Socket created: " << sockid << '\n';
        ASSERT_NE(srt_epoll_add_usock(poll_id, sockid, &epoll_out), SRT_ERROR);
    }
    {
        const int sockid = srt_connect(ss, (sockaddr*) &safail, sizeof safail);
        EXPECT_GT(sockid, 0) << "Socket " << 2;
        safail.sin_port = htons(4201); // Changing port so that second connect fails
        std::cout << "Socket created: " << sockid << '\n';
        ASSERT_NE(srt_epoll_add_usock(poll_id, sockid, &epoll_out), SRT_ERROR);
    }
    std::cout << "Returned from connecting two sockets " << std::endl;

    const int default_len = 3;
    int rlen = default_len;
    SRTSOCKET read[default_len];

    int wlen = default_len;
    SRTSOCKET write[default_len];

    for (int j = 0; j < 2; ++j)
    {
        const int epoll_res = srt_epoll_wait(poll_id, read, &rlen,
            write, &wlen,
            5000, /* timeout */
            0, 0, 0, 0);
            
        std::cout << "Epoll result: " << epoll_res << '\n';
        std::cout << "Epoll rlen: " << rlen << ", wlen: " << wlen << '\n';
        for (int i = 0; i < rlen; ++i)
        {
            std::cout << "Epoll read[" << i << "]: " << read[i] << '\n';
        }
        for (int i = 0; i < wlen; ++i)
        {
            std::cout << "Epoll write[" << i << "]: " << write[i] << " (removed from epoll)\n";
            EXPECT_EQ(srt_epoll_remove_usock(poll_id, write[i]), 0);
        }
    }

    listen_promise.wait();

    EXPECT_EQ(srt_close(ss), 0) << "srt_close: %s\n" << srt_getlasterror_str();
}

void ConnectCallback_Close(void* /*opaq*/, SRTSOCKET sock, int error, const sockaddr* /*peer*/, int token)
{
    std::cout << "Connect callback. Socket: " << sock
        << ", error: " << error
        << ", token: " << token << '\n';

    if (error == SRT_SUCCESS)
        return;

    // XXX WILL CAUSE DEADLOCK!
    srt_close(sock);
}

TEST(Bonding, CloseGroupAndSocket)
{
    srt::TestInit srtinit;

    const int ss = srt_create_group(SRT_GTYPE_BROADCAST);
    ASSERT_NE(ss, SRT_ERROR);
    std::cout << "Created group socket: " << ss << '\n';

    int no = 0;
    ASSERT_NE(srt_setsockopt(ss, 0, SRTO_RCVSYN, &no, sizeof no), SRT_ERROR); // non-blocking mode
    ASSERT_NE(srt_setsockopt(ss, 0, SRTO_SNDSYN, &no, sizeof no), SRT_ERROR); // non-blocking mode

    const int poll_id = srt_epoll_create();
    // Will use this epoll to wait for srt_accept(...)
    const int epoll_out = SRT_EPOLL_OUT | SRT_EPOLL_ERR;
    ASSERT_NE(srt_epoll_add_usock(poll_id, ss, &epoll_out), SRT_ERROR);

    srt_connect_callback(ss, &ConnectCallback_Close, this);

    sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(4200);
    ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

    std::future<void> listen_promise = std::async(std::launch::async, std::bind(listening_thread, true));
    
    std::cout << "Connecting two sockets " << std::endl;
    for (int i = 0; i < 2; ++i)
    {
        const int sockid = srt_connect(ss, (sockaddr*) &sa, sizeof sa);
        EXPECT_GT(sockid, 0) << "Socket " << i;
        sa.sin_port = htons(4201); // Changing port so that second connect fails
        std::cout << "Socket created: " << sockid << '\n';
        ASSERT_NE(srt_epoll_add_usock(poll_id, sockid, &epoll_out), SRT_ERROR);
    }
    std::cout << "Returned from connecting two sockets " << std::endl;

    for (int j = 0; j < 2; ++j)
    {
        const int default_len = 3;
        int rlen = default_len;
        SRTSOCKET read[default_len];

        int wlen = default_len;
        SRTSOCKET write[default_len];

        const int epoll_res = srt_epoll_wait(poll_id, read, &rlen,
            write, &wlen,
            5000, /* timeout */
            0, 0, 0, 0);

        std::cout << "Epoll result: " << epoll_res << '\n';
        std::cout << "Epoll rlen: " << rlen << ", wlen: " << wlen << '\n';
        if (epoll_res < 0)
            continue;

        for (int i = 0; i < rlen; ++i)
        {
            std::cout << "Epoll read[" << i << "]: " << read[i] << '\n';
        }
        for (int i = 0; i < wlen; ++i)
        {
            std::cout << "Epoll write[" << i << "]: " << write[i] << " (removed from epoll)\n";
            EXPECT_EQ(srt_epoll_remove_usock(poll_id, write[i]), 0);
        }
    }

    // Some basic checks for group stats
    SRT_TRACEBSTATS stats;
    EXPECT_EQ(srt_bstats(ss, &stats, true), SRT_SUCCESS);
    EXPECT_EQ(stats.pktSent, 0);
    EXPECT_EQ(stats.pktSentTotal, 0);
    EXPECT_EQ(stats.pktSentUnique, 0);
    EXPECT_EQ(stats.pktSentUniqueTotal, 0);
    EXPECT_EQ(stats.pktRecv, 0);
    EXPECT_EQ(stats.pktRecvTotal, 0);
    EXPECT_EQ(stats.pktRecvUnique, 0);
    EXPECT_EQ(stats.pktRecvUniqueTotal, 0);
    EXPECT_EQ(stats.pktRcvDrop, 0);
    EXPECT_EQ(stats.pktRcvDropTotal, 0);

    std::cout << "Starting thread for sending:\n";
    std::thread sender([ss] {
        char buf[1316];
        memset(buf, 1, sizeof(buf));
        int n = 0;
        for (int i = 0; i < 10000; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (srt_send(ss, buf, 1316) == -1)
            {
                std::cout << "[Sender] sending failure, exitting after sending " << n << " packets\n";
                break;
            }

            ++n;
        }
    });

    std::cout << "Will close sending in 300ms...\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_EQ(srt_close(ss), 0) << "srt_close: %s\n" << srt_getlasterror_str();

    std::cout << "CLOSED GROUP. Now waiting for sender to exit...\n";
    sender.join();
    listen_promise.wait();
}

// (void* opaq, SRTSOCKET ns, int hsversion, const struct sockaddr* peeraddr, const char* streamid);
int ListenCallbackFn(void* expected_sid, SRTSOCKET, int /*hsversion*/, const sockaddr* /*peer*/, const char* streamid)
{
    const auto* p = (std::pair<const char*, int>*) expected_sid;
    // Note: It is not safe to access the streamid pointer by the expected size,
    // but there is no way to know the real size apart from finding the first null terminator.
    // See FR #3073.
    EXPECT_EQ(std::memcmp(streamid, p->first, p->second), 0);
    
    return 0;
}

TEST(Bonding, Options)
{
    using namespace std;
    using namespace srt;

    TestInit srtinit;

    // Create a group
    const SRTSOCKET grp = srt_create_group(SRT_GTYPE_BROADCAST);

    // rendezvous shall not be allowed to be set on the group
    // XXX actually it is possible, but no one tested it. POSTPONE.
    //int yes = 1;
    //EXPECT_EQ(srt_setsockflag(grp, SRTO_RENDEZVOUS, &yes, sizeof yes), SRT_ERROR);

#ifdef SRT_ENABLE_ENCRYPTION
    const string pass = "longenoughpassword";
    // passphrase should be ok.
    EXPECT_NE(srt_setsockflag(grp, SRTO_PASSPHRASE, pass.c_str(), (int) pass.size()), SRT_ERROR);

    uint32_t val = 16;
    EXPECT_NE(srt_setsockflag(grp, SRTO_PBKEYLEN, &val, (int) sizeof val), SRT_ERROR);

#ifdef ENABLE_AEAD_API_PREVIEW
    val = 1;
    EXPECT_NE(srt_setsockflag(grp, SRTO_CRYPTOMODE, &val, sizeof val), SRT_ERROR);
#endif
#endif

    // ================
    // Linger is an option of a trivial type, but differes from other integer-typed options.
    // Therefore checking it specifically.
    const linger l = {1, 10};
    srt_setsockflag(grp, SRTO_LINGER, &l, sizeof l);

    {
        linger l2;
        int optsize = sizeof l2;
        EXPECT_NE(srt_getsockflag(grp, SRTO_LINGER, &l2, &optsize), SRT_ERROR);
        EXPECT_EQ(optsize, (int)sizeof l2);
        EXPECT_EQ(l2.l_onoff, l.l_onoff);
        EXPECT_EQ(l2.l_linger, l.l_linger);
    }
    // ================

    const std::array<char, 10> streamid = { 's', 't', 'r', 'e', 0, 'm', 'i', 'd', '%', '&'};
    EXPECT_NE(srt_setsockflag(grp, SRTO_STREAMID, &streamid, streamid.size()), SRT_ERROR);

    auto check_streamid = [&streamid](SRTSOCKET sock) {
        std::array<char, srt::CSrtConfig::MAX_SID_LENGTH> tmpbuf;
        auto opt_len = (int)tmpbuf.size();
        EXPECT_EQ(srt_getsockflag(sock, SRTO_STREAMID, tmpbuf.data(), &opt_len), SRT_SUCCESS);
        EXPECT_EQ(size_t(opt_len), streamid.size());
        EXPECT_EQ(std::memcmp(tmpbuf.data(), streamid.data(), opt_len), 0);
    };

    check_streamid(grp);

    int lat = 500;
    EXPECT_NE(srt_setsockflag(grp, SRTO_RCVLATENCY, &lat, sizeof lat), SRT_ERROR);

    mutex mx;
    condition_variable latch;
    atomic<bool> started {false};

    thread accept_and_close { [&]() {

        unique_lock<mutex> ux(mx);

        SRTSOCKET lsn = srt_create_socket();

        auto expected_sid = std::make_pair<const char*, int>(streamid.data(), streamid.size());
        srt_listen_callback(lsn, &ListenCallbackFn, (void*) &expected_sid);

#ifdef SRT_ENABLE_ENCRYPTION
        EXPECT_NE(srt_setsockflag(lsn, SRTO_PASSPHRASE, pass.c_str(), pass.size()), SRT_ERROR);
#endif
        int allow = 1;
        ASSERT_NE(srt_setsockflag(lsn, SRTO_GROUPCONNECT, &allow, sizeof allow), SRT_ERROR);
        sockaddr_any sa = CreateAddr("127.0.0.1", 5555, AF_INET);
        ASSERT_NE(srt_bind(lsn, sa.get(), sa.size()), SRT_ERROR);
        ASSERT_NE(srt_listen(lsn, 1), SRT_ERROR);
        started = true;

        // First wait - until it's let go with accepting
        latch.wait(ux);

        sockaddr_any revsa;
        SRTSOCKET gs = srt_accept(lsn, revsa.get(), &revsa.len);
        ASSERT_NE(gs, SRT_ERROR);

        check_streamid(gs);

        // Connected, wait to close
        latch.wait(ux);

        srt_close(gs);
        srt_close(lsn);
    }};

    // Give the thread a chance to start
    this_thread::yield();

    while (!started)
    {
        // In case of a bad luck, just wait for the thread to
        // acquire the mutex before you do
        this_thread::sleep_for(chrono::milliseconds(10));
    }

    // Wait for the possibility to connect
    {
        // Make sure that the thread reached the wait() call.
        unique_lock<mutex> ux(mx);
        latch.notify_all();
    }

    // Now the thread is accepting, so we call the connect.
    sockaddr_any sa = CreateAddr("127.0.0.1", 5555, AF_INET);
    SRTSOCKET member = srt_connect(grp, sa.get(), sa.size());

    // We've released the mutex and signaled the CV, so accept should proceed now.
    // Exit from srt_connect() means also exit from srt_accept().

    EXPECT_NE(member, SRT_INVALID_SOCK);

    // conenct_res should be a socket
    EXPECT_NE(member, 0); // XXX Change to SRT_SOCKID_CONNREQ

    // Now get the option value from the group

    int revlat = -1;
    int optsize = sizeof revlat;
    EXPECT_NE(srt_getsockflag(grp, SRTO_RCVLATENCY, &revlat, &optsize), SRT_ERROR);
    EXPECT_EQ(optsize, (int) sizeof revlat);
    EXPECT_EQ(revlat, 500);

    revlat = -1;
    optsize = sizeof revlat;
    // Expect the same value set on the member socket
    EXPECT_NE(srt_getsockflag(member, SRTO_RCVLATENCY, &revlat, &optsize), SRT_ERROR);
    EXPECT_EQ(optsize, (int) sizeof revlat);
    EXPECT_EQ(revlat, 500);

    // Individual socket option modified on group
    int ohead = 12;
    optsize = sizeof ohead;
    EXPECT_NE(srt_setsockflag(grp, SRTO_OHEADBW, &ohead, optsize), SRT_ERROR);

    // Modifyting a post-option should be possible on a socket
    ohead = 11;
    optsize = sizeof ohead;
    EXPECT_NE(srt_setsockflag(member, SRTO_OHEADBW, &ohead, optsize), SRT_ERROR);

    // But getting the option value should be equal to the group setting
    EXPECT_NE(srt_getsockflag(grp, SRTO_OHEADBW, &ohead, &optsize), SRT_ERROR);
    EXPECT_EQ(optsize, (int) sizeof ohead);
    EXPECT_EQ(ohead, 12);

#if SRT_ENABLE_ENCRYPTION

    uint32_t kms = -1;

    EXPECT_NE(srt_getsockflag(grp, SRTO_KMSTATE, &kms, &optsize), SRT_ERROR);
    EXPECT_EQ(optsize, (int) sizeof kms);
    EXPECT_EQ(kms, int(SRT_KM_S_SECURED));

    EXPECT_NE(srt_getsockflag(grp, SRTO_PBKEYLEN, &kms, &optsize), SRT_ERROR);
    EXPECT_EQ(optsize, (int) sizeof kms);
    EXPECT_EQ(kms, 16);

#ifdef ENABLE_AEAD_API_PREVIEW
    EXPECT_NE(srt_getsockflag(grp, SRTO_CRYPTOMODE, &kms, &optsize), SRT_ERROR);
    EXPECT_EQ(optsize, sizeof kms);
    EXPECT_EQ(kms, 1);
#endif
#endif

    // We're done, the thread can close connection and exit
    {
        // Make sure that the thread reached the wait() call.
        std::unique_lock<std::mutex> ux(mx);
        latch.notify_all();
    }

    accept_and_close.join();
    srt_close(grp);
}

inline SRT_SOCKGROUPCONFIG PrepareEndpoint(const std::string& host, int port)
{
    srt::sockaddr_any sa = srt::CreateAddr(host, port, AF_INET);
    return srt_prepare_endpoint(NULL, sa.get(), sa.size());
}

// This test will create a listener and then the group that should
// connect members, where the first one fail, and two next should
// succeed. Then sends a single packet over that link and makes sure
// it's properly received, then the second packet isn't read.
TEST(Bonding, InitialFailure)
{
    using namespace std;
    using namespace srt;

    TestInit srtinit;
    MAKE_UNIQUE_SOCK(lsn, "Listener", srt_create_socket());
    MAKE_UNIQUE_SOCK(grp, "GrpCaller", srt_create_group(SRT_GTYPE_BROADCAST));

    // Create the listener on port 5555.
    int allow = 1;
    ASSERT_NE(srt_setsockflag(lsn, SRTO_GROUPCONNECT, &allow, sizeof allow), SRT_ERROR);

    sockaddr_any sa = CreateAddr("127.0.0.1", 5555, AF_INET);
    ASSERT_NE(srt_bind(lsn, sa.get(), sa.size()), SRT_ERROR);
    ASSERT_NE(srt_listen(lsn, 5), SRT_ERROR);

    // Create a group
    // Connect 3 members in the group.
    std::vector<SRT_SOCKGROUPCONFIG> targets;
    targets.push_back(PrepareEndpoint("127.0.0.1", 5556)); // NOTE: NONEXISTENT LISTENER
    targets.push_back(PrepareEndpoint("127.0.0.1", 5555));
    targets.push_back(PrepareEndpoint("127.0.0.1", 5555));

    // This should block until the connection is established, but
    // accepted socket should be spawned and just wait for extraction.
    const SRTSOCKET conn = srt_connect_group(grp, targets.data(), (int)targets.size());
    EXPECT_NE(conn, SRT_INVALID_SOCK);

    // Now check if the accept is ready
    sockaddr_any revsa;
    const SRTSOCKET gs = srt_accept(lsn, revsa.get(), &revsa.len);
    EXPECT_NE(gs, SRT_INVALID_SOCK);

    // Make sure that it was the group accepted
    EXPECT_EQ(gs & SRTGROUP_MASK, SRTGROUP_MASK);

    // Set 1s reading timeout on the socket so that reading won't wait forever,
    // as it should fail at the second reading.
    int read_timeout = 500; // 0.5s
    EXPECT_NE(srt_setsockflag(gs, SRTO_RCVTIMEO, &read_timeout, sizeof (read_timeout)), SRT_ERROR);

    int lsn_isn = -1, lsn_isn_size = sizeof (int);
    EXPECT_NE(srt_getsockflag(gs, SRTO_ISN, &lsn_isn, &lsn_isn_size), SRT_ERROR);

    // Now send a packet

    string packet_data = "PREDEFINED PACKET DATA";
    EXPECT_NE(srt_send(grp, packet_data.data(), packet_data.size()), SRT_ERROR);

    char outbuf[1316];
    SRT_MSGCTRL mc = srt_msgctrl_default;
    int recvlen = srt_recvmsg2(gs, outbuf, 1316, &mc);
    EXPECT_EQ(recvlen, int(packet_data.size()));

    if (recvlen > 0)
    {
        outbuf[recvlen] = 0;
        EXPECT_EQ(outbuf, packet_data);
    }
    EXPECT_EQ(mc.pktseq, lsn_isn);

    recvlen = srt_recv(gs, outbuf, 80);
    EXPECT_EQ(recvlen, int(SRT_ERROR));

    srt_close(gs);
    srt_close(grp);
    srt_close(lsn);
}

