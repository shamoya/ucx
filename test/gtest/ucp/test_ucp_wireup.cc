/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#define __STDC_LIMIT_MACROS

#include "ucp_test.h"

#include <algorithm>

extern "C" {
#include <ucp/wireup/address.h>
#include <ucp/proto/proto.h>
}

class test_ucp_wireup : public ucp_test {
public:
    static std::vector<ucp_test_param>
    enum_test_params(const ucp_params_t& ctx_params,
                     const std::string& name,
                     const std::string& test_case_name,
                     const std::string& tls);

protected:
    enum {
        TEST_RMA,
        TEST_TAG
    };

    typedef uint64_t               elem_type;
    typedef std::vector<elem_type> vec_type;

    static const size_t BUFFER_LENGTH    = 16384;
    static const ucp_datatype_t DT_U64 = ucp_dt_make_contig(sizeof(elem_type));
    static const uint64_t TAG          = 0xdeadbeef;
    static const elem_type SEND_DATA   = 0xdeadbeef12121212ull;

    virtual void init();
    virtual void cleanup();

    void send_nb(ucp_ep_h ep, size_t length, int repeat, std::vector<void*>& reqs);

    void send_b(ucp_ep_h ep,         size_t length, int repeat);

    void recv_b(ucp_worker_h worker, size_t length, int repeat);

    void send_recv(ucp_ep_h ep, ucp_worker_h worker, size_t vecsize, int repeat);

    void disconnect(ucp_ep_h ep);

    void waitall(std::vector<void*> reqs);

private:
    vec_type   m_send_data;
    vec_type   m_recv_data;
    size_t     m_max_send;
    ucp_mem_h  m_memh1, m_memh2;
    ucp_rkey_h m_rkey1, m_rkey2;

    void clear_recv_data();

    ucp_rkey_h get_rkey(ucp_mem_h memh);

    static void send_completion(void *request, ucs_status_t status);

    static void recv_completion(void *request, ucs_status_t status,
                                ucp_tag_recv_info_t *info);
};

std::vector<ucp_test_param>
test_ucp_wireup::enum_test_params(const ucp_params_t& ctx_params,
                                  const std::string& name,
                                  const std::string& test_case_name,
                                  const std::string& tls)
{
    std::vector<ucp_test_param> result;
    ucp_params_t tmp_ctx_params = ctx_params;

    tmp_ctx_params.features = UCP_FEATURE_RMA;
    generate_test_params_variant(tmp_ctx_params, name, test_case_name + "/rma",
                                 tls, TEST_RMA, result);

    tmp_ctx_params.features = UCP_FEATURE_TAG;
    generate_test_params_variant(tmp_ctx_params, name, test_case_name + "/tag",
                                 tls, TEST_TAG, result);

    return result;
}

void test_ucp_wireup::init() {
    ucp_test::init();

    m_send_data.resize(BUFFER_LENGTH, elem_type(SEND_DATA));
    m_recv_data.resize(BUFFER_LENGTH, 0);

    if (GetParam().variant == UCP_FEATURE_RMA) {
        ucs_status_t status;

        void *ptr   = &m_recv_data[0];
        size_t size = m_recv_data.size() * sizeof(m_recv_data[0]);

        status = ucp_mem_map(receiver().ucph(), &ptr, size, 0, &m_memh1);
        ASSERT_UCS_OK(status);

        status = ucp_mem_map(sender().ucph(), &ptr, size, 0, &m_memh2);
        ASSERT_UCS_OK(status);

        m_rkey1 = get_rkey(m_memh1);
        m_rkey2 = get_rkey(m_memh2);
    }
}

ucp_rkey_h test_ucp_wireup::get_rkey(ucp_mem_h memh)
{
    void *rkey_buffer;
    size_t rkey_size;
    ucs_status_t status;
    ucp_rkey_h rkey;

    status = ucp_rkey_pack(receiver().ucph(), memh, &rkey_buffer, &rkey_size);
    ASSERT_UCS_OK(status);

    status = ucp_ep_rkey_unpack(sender().ep(), rkey_buffer, &rkey);
    ASSERT_UCS_OK(status);

    ucp_rkey_buffer_release(rkey_buffer);

    return rkey;
}

void test_ucp_wireup::cleanup() {
    if (GetParam().variant == UCP_FEATURE_RMA) {
        ucp_rkey_destroy(m_rkey1);
        ucp_mem_unmap(receiver().ucph(), m_memh1);
        ucp_rkey_destroy(m_rkey2);
        ucp_mem_unmap(sender().ucph(), m_memh2);
    }
    ucp_test::cleanup();
}

void test_ucp_wireup::clear_recv_data() {
    std::fill(m_recv_data.begin(), m_recv_data.end(), 0);
}

void test_ucp_wireup::send_nb(ucp_ep_h ep, size_t length, int repeat,
                              std::vector<void*>& reqs)
{
    if (GetParam().variant == UCP_FEATURE_TAG) {
        for (int i = 0; i < repeat; ++i) {
            void *req = ucp_tag_send_nb(ep, &m_send_data[0], length,
                                        DT_U64, TAG, send_completion);
            if (UCS_PTR_IS_PTR(req)) {
                reqs.push_back(req);
            } else {
                ASSERT_UCS_OK(UCS_PTR_STATUS(req));
            }
        }
    } else if (GetParam().variant == UCP_FEATURE_RMA) {
        clear_recv_data();
        for (int i = 0; i < repeat; ++i) {
            std::fill(m_send_data.begin(), m_send_data.end(), SEND_DATA + i);
            ucs_status_t status;
            status = ucp_put(ep, &m_send_data[0],
                             m_send_data.size() * sizeof(m_send_data[0]),
                             (uintptr_t)&m_recv_data[0],
                             (sender().ep() == ep) ? m_rkey1 : m_rkey2);
            ASSERT_UCS_OK(status);
        }
    }
}

void test_ucp_wireup::send_b(ucp_ep_h ep, size_t length, int repeat)
{
    std::vector<void*> reqs;
    send_nb(ep, length, repeat, reqs);
    waitall(reqs);
}

void test_ucp_wireup::recv_b(ucp_worker_h worker, size_t length, int repeat)
{
    if (GetParam().variant == UCP_FEATURE_TAG) {
        for (int i = 0; i < repeat; ++i) {
            clear_recv_data();
            void *req = ucp_tag_recv_nb(worker, &m_recv_data[0], length,
                                        DT_U64, TAG, (ucp_tag_t)-1,
                                        recv_completion);
            if (UCS_PTR_IS_PTR(req)) {
                wait(req);
            } else {
                ASSERT_UCS_OK(UCS_PTR_STATUS(req));
            }
            EXPECT_EQ(length,
                      (size_t)std::count(m_recv_data.begin(), m_recv_data.begin() + length,
                                         elem_type(SEND_DATA)));
        }
    } else if (GetParam().variant == UCP_FEATURE_RMA) {
        for (size_t i = 0; i < length; ++i) {
            while (m_recv_data[i] != SEND_DATA + repeat);
        }
    }
}

void test_ucp_wireup::send_completion(void *request, ucs_status_t status)
{
}

void test_ucp_wireup::recv_completion(void *request, ucs_status_t status,
                                      ucp_tag_recv_info_t *info)
{
}

void test_ucp_wireup::send_recv(ucp_ep_h ep, ucp_worker_h worker,
                                size_t length, int repeat)
{
    std::vector<void*> send_reqs;
    send_nb(ep,     length, repeat, send_reqs);
    recv_b (worker, length, repeat);
    waitall(send_reqs);
}

void test_ucp_wireup::disconnect(ucp_ep_h ep) {
    void *req = ucp_disconnect_nb(ep);
    if (!UCS_PTR_IS_PTR(req)) {
        ASSERT_UCS_OK(UCS_PTR_STATUS(req));
    }
    wait(req);
}

void test_ucp_wireup::waitall(std::vector<void*> reqs)
{
    while (!reqs.empty()) {
        wait(reqs.back());
        reqs.pop_back();
    }
}

UCS_TEST_P(test_ucp_wireup, address) {
    ucs_status_t status;
    size_t size;
    void *buffer;
    unsigned order[UCP_MAX_RESOURCES];

    status = ucp_address_pack(sender().worker(), NULL, -1, order, &size, &buffer);
    ASSERT_UCS_OK(status);
    ASSERT_TRUE(buffer != NULL);
    ASSERT_GT(size, 0ul);
    EXPECT_LE(size, 512ul); /* Expect a reasonable address size */

    char name[UCP_WORKER_NAME_MAX];
    uint64_t uuid;
    unsigned address_count;
    ucp_address_entry_t *address_list;

    ucp_address_unpack(buffer, &uuid, name, sizeof(name), &address_count,
                       &address_list);
    EXPECT_EQ(sender().worker()->uuid, uuid);
    EXPECT_EQ(std::string(ucp_worker_get_name(sender().worker())), std::string(name));
    EXPECT_LE(address_count, static_cast<unsigned>(sender().ucph()->num_tls));

    /* TODO test addresses */

    ucs_free(address_list);
    ucs_free(buffer);
}

UCS_TEST_P(test_ucp_wireup, empty_address) {
    ucs_status_t status;
    size_t size;
    void *buffer;
    unsigned order[UCP_MAX_RESOURCES];

    status = ucp_address_pack(sender().worker(), NULL, 0, order, &size, &buffer);
    ASSERT_UCS_OK(status);
    ASSERT_TRUE(buffer != NULL);
    ASSERT_GT(size, 0ul);

    char name[UCP_WORKER_NAME_MAX];
    uint64_t uuid;
    unsigned address_count;
    ucp_address_entry_t *address_list;

    ucp_address_unpack(buffer, &uuid, name, sizeof(name), &address_count,
                       &address_list);
    EXPECT_EQ(sender().worker()->uuid, uuid);
    EXPECT_EQ(std::string(ucp_worker_get_name(sender().worker())), std::string(name));
    EXPECT_LE(address_count, sender().ucph()->num_tls);
    EXPECT_EQ(0u, address_count);

    ucs_free(address_list);
    ucs_free(buffer);
}

UCS_TEST_P(test_ucp_wireup, one_sided_wireup) {
    sender().connect(&receiver());
    send_recv(sender().ep(), receiver().worker(), 1, 1);
    sender().flush_worker();
}

UCS_TEST_P(test_ucp_wireup, two_sided_wireup) {
    sender().connect(&receiver());
    if (&sender() != &receiver()) {
        receiver().connect(&sender());
    }

    send_recv(sender().ep(), receiver().worker(), 1, 1);
    sender().flush_worker();
    send_recv(receiver().ep(), sender().worker(), 1, 1);
    receiver().flush_worker();
}

UCS_TEST_P(test_ucp_wireup, multi_wireup) {
    skip_loopback();

    const size_t count = 10;
    while (entities().size() < count) {
        create_entity();
    }

    /* connect from sender() to all the rest */
    for (size_t i = 0; i < count; ++i) {
        sender().connect(&entities().at(i));
    }
}

UCS_TEST_P(test_ucp_wireup, reply_ep_send_before) {
    skip_loopback();

    sender().connect(&receiver());

    if (GetParam().variant == TEST_TAG) {
        /* Send a reply */
        ucp_ep_connect_remote(sender().ep());
        ucp_ep_h ep = ucp_worker_get_reply_ep(receiver().worker(),
                                              sender().worker()->uuid);
        send_recv(ep, sender().worker(), 1, 1);
        sender().flush_worker();

        ucp_ep_destroy(ep);
    }
}

UCS_TEST_P(test_ucp_wireup, reply_ep_send_after) {
    skip_loopback();

    sender().connect(&receiver());

    if (GetParam().variant == TEST_TAG) {
        ucp_ep_connect_remote(sender().ep());

        /* Make sure the wireup message arrives before sending a reply */
        send_recv(sender().ep(), receiver().worker(), 1, 1);
        sender().flush_worker();

        /* Send a reply */
        ucp_ep_h ep = ucp_worker_get_reply_ep(receiver().worker(), sender().worker()->uuid);
        send_recv(ep, sender().worker(), 1, 1);

        sender().flush_worker();

        ucp_ep_destroy(ep);
    }
}

UCS_TEST_P(test_ucp_wireup, stress_connect) {
    for (int i = 0; i < 30; ++i) {
        sender().connect(&receiver());
        send_recv(sender().ep(), receiver().worker(), 1,
                  10000 / ucs::test_time_multiplier());
        if (!is_loopback()) {
            receiver().connect(&sender());
        }

        disconnect(sender().revoke_ep());
        if (!is_loopback()) {
            disconnect(receiver().revoke_ep());
        }
    }
}

UCS_TEST_P(test_ucp_wireup, stress_connect2) {
    for (int i = 0; i < 1000 / ucs::test_time_multiplier(); ++i) {
        sender().connect(&receiver());
        send_recv(sender().ep(), receiver().worker(), 1, 1);
        if (&sender() != &receiver()) {
            receiver().connect(&sender());
        }

        disconnect(sender().revoke_ep());
        if (!is_loopback()) {
            disconnect(receiver().revoke_ep());
        }
    }
}

UCS_TEST_P(test_ucp_wireup, connect_disconnect) {
    sender().connect(&receiver());
    if (!is_loopback()) {
        receiver().connect(&sender());
    }
    test_ucp_wireup::disconnect(sender().revoke_ep());
    if (!is_loopback()) {
        receiver().disconnect();
    }
}

UCS_TEST_P(test_ucp_wireup, disconnect_nonexistent) {
    sender().connect(&receiver());
    sender().disconnect();
    receiver().destroy_worker();
    sender().destroy_worker();
}

UCS_TEST_P(test_ucp_wireup, disconnect_reconnect) {
    sender().connect(&receiver());
    send_b(sender().ep(), 1000, 1);
    sender().disconnect();
    recv_b(receiver().worker(), 1000, 1);

    sender().connect(&receiver());
    send_b(sender().ep(), 1000, 1);
    sender().disconnect();
    recv_b(receiver().worker(), 1000, 1);
}

UCS_TEST_P(test_ucp_wireup, send_disconnect_onesided) {
    sender().connect(&receiver());
    send_b(sender().ep(), 1000, 100);
    sender().disconnect();
    recv_b(receiver().worker(), 1000, 100);
}

UCS_TEST_P(test_ucp_wireup, send_disconnect_onesided_nozcopy, "ZCOPY_THRESH=-1") {
    sender().connect(&receiver());
    send_b(sender().ep(), 1000, 100);
    sender().disconnect();
    recv_b(receiver().worker(), 1000, 100);
}

UCS_TEST_P(test_ucp_wireup, send_disconnect_reply1) {
    sender().connect(&receiver());
    if (!is_loopback()) {
        receiver().connect(&sender());
    }

    send_b(sender().ep(), 8, 1);
    if (!is_loopback()) {
        sender().disconnect();
    }

    recv_b(receiver().worker(), 8, 1);
    send_b(receiver().ep(), 8, 1);
    receiver().disconnect();
    recv_b(sender().worker(), 8, 1);
}

UCS_TEST_P(test_ucp_wireup, send_disconnect_reply2) {
    sender().connect(&receiver());

    send_b(sender().ep(), 8, 1);
    if (!is_loopback()) {
        sender().disconnect();
    }
    recv_b(receiver().worker(), 8, 1);

    if (!is_loopback()) {
        receiver().connect(&sender());
    }

    send_b(receiver().ep(), 8, 1);
    receiver().disconnect();
    recv_b(sender().worker(), 8, 1);
}

UCS_TEST_P(test_ucp_wireup, send_disconnect_onesided_wait) {
    sender().connect(&receiver());
    send_recv(sender().ep(), receiver().worker(), 8, 1);
    send_b(sender().ep(), 1000, 200);
    sender().disconnect();
    recv_b(receiver().worker(), 1000, 200);
}

UCS_TEST_P(test_ucp_wireup, disconnect_nb_onesided) {
    sender().connect(&receiver());

    std::vector<void*> sreqs;
    send_nb(sender().ep(), 1000, 1000, sreqs);

    void *dreq = sender().disconnect_nb();
    if (!UCS_PTR_IS_PTR(dreq)) {
        ASSERT_UCS_OK(UCS_PTR_STATUS(dreq));
    }

    wait(dreq);
    recv_b(receiver().worker(), 1000, 1000);

    waitall(sreqs);

}

UCP_INSTANTIATE_TEST_CASE(test_ucp_wireup)
