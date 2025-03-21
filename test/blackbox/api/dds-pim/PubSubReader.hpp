// Copyright 2016, 2020 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file PubSubReader.hpp
 *
 */

#ifndef _TEST_BLACKBOX_PUBSUBREADER_HPP_
#define _TEST_BLACKBOX_PUBSUBREADER_HPP_

#include <atomic>
#include <condition_variable>
#include <list>
#include <string>

#include <asio.hpp>
#include <gtest/gtest.h>
#if _MSC_VER
#include <Windows.h>
#endif // _MSC_VER
#include <fastdds/dds/core/condition/StatusCondition.hpp>
#include <fastdds/dds/core/condition/GuardCondition.hpp>
#include <fastdds/dds/core/condition/WaitSet.hpp>
#include <fastdds/dds/core/UserAllocatedSequence.hpp>
#include <fastdds/dds/core/policy/QosPolicies.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipantListener.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/rtps/transport/UDPTransportDescriptor.h>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.h>
#include <fastdds/rtps/transport/UDPv6TransportDescriptor.h>
#include <fastrtps/xmlparser/XMLParser.h>
#include <fastrtps/xmlparser/XMLTree.h>
#include <fastrtps/utils/IPLocator.h>

using DomainParticipantFactory = eprosima::fastdds::dds::DomainParticipantFactory;
using eprosima::fastrtps::rtps::IPLocator;
using eprosima::fastdds::rtps::UDPTransportDescriptor;
using eprosima::fastdds::rtps::UDPv4TransportDescriptor;
using eprosima::fastdds::rtps::UDPv6TransportDescriptor;

template<class TypeSupport>
class PubSubReader
{
public:

    typedef TypeSupport type_support;
    typedef typename type_support::type type;

protected:

    class ParticipantListener : public eprosima::fastdds::dds::DomainParticipantListener
    {
    public:

        ParticipantListener(
                PubSubReader& reader)
            : reader_(reader)
        {
        }

        ~ParticipantListener()
        {
        }

        void on_participant_discovery(
                eprosima::fastdds::dds::DomainParticipant*,
                eprosima::fastrtps::rtps::ParticipantDiscoveryInfo&& info) override
        {
            if (reader_.onDiscovery_ != nullptr)
            {
                std::unique_lock<std::mutex> lock(reader_.mutexDiscovery_);
                reader_.discovery_result_ |= reader_.onDiscovery_(info);
                reader_.cvDiscovery_.notify_one();
            }

            if (info.status == eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::DISCOVERED_PARTICIPANT)
            {
                reader_.participant_matched();

            }
            else if (info.status == eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::REMOVED_PARTICIPANT ||
                    info.status == eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::DROPPED_PARTICIPANT)
            {
                reader_.participant_unmatched();
            }
        }

        void on_publisher_discovery(
                eprosima::fastdds::dds::DomainParticipant*,
                eprosima::fastrtps::rtps::WriterDiscoveryInfo&& info) override
        {
            if (reader_.onEndpointDiscovery_ != nullptr)
            {
                std::unique_lock<std::mutex> lock(reader_.mutexDiscovery_);
                reader_.discovery_result_ |= reader_.onEndpointDiscovery_(info);
                reader_.cvDiscovery_.notify_one();
            }
        }

#if HAVE_SECURITY
        void onParticipantAuthentication(
                eprosima::fastdds::dds::DomainParticipant*,
                eprosima::fastrtps::rtps::ParticipantAuthenticationInfo&& info) override
        {
            if (info.status == eprosima::fastrtps::rtps::ParticipantAuthenticationInfo::AUTHORIZED_PARTICIPANT)
            {
                reader_.authorized();
            }
            else if (info.status == eprosima::fastrtps::rtps::ParticipantAuthenticationInfo::UNAUTHORIZED_PARTICIPANT)
            {
                reader_.unauthorized();
            }
        }

#endif // if HAVE_SECURITY

    private:

        ParticipantListener& operator =(
                const ParticipantListener&) = delete;
        PubSubReader& reader_;

    }
    participant_listener_;

    class Listener : public eprosima::fastdds::dds::DataReaderListener
    {
    public:

        Listener(
                PubSubReader& reader)
            : reader_(reader)
            , times_deadline_missed_(0)
        {
        }

        ~Listener()
        {
        }

        void on_data_available(
                eprosima::fastdds::dds::DataReader* datareader) override
        {
            ASSERT_NE(datareader, nullptr);
            {
                std::lock_guard<std::mutex> guard(reader_.message_receive_mutex_);
                reader_.message_receive_count_.fetch_add(1);
            }
            reader_.message_receive_cv_.notify_one();

            if (reader_.receiving_.load())
            {
                bool ret = false;
                do
                {
                    reader_.receive_one(datareader, ret);
                } while (ret);
            }
        }

        void on_subscription_matched(
                eprosima::fastdds::dds::DataReader* /*datareader*/,
                const eprosima::fastdds::dds::SubscriptionMatchedStatus& info) override
        {
            if (0 < info.current_count_change)
            {
                std::cout << "Subscriber matched publisher " << info.last_publication_handle << std::endl;
                reader_.matched();
            }
            else
            {
                std::cout << "Subscriber unmatched publisher " << info.last_publication_handle << std::endl;
                reader_.unmatched();
            }
        }

        void on_requested_deadline_missed(
                eprosima::fastdds::dds::DataReader* datareader,
                const eprosima::fastrtps::RequestedDeadlineMissedStatus& status) override
        {
            (void)datareader;

            times_deadline_missed_ = status.total_count;
        }

        void on_requested_incompatible_qos(
                eprosima::fastdds::dds::DataReader* datareader,
                const eprosima::fastdds::dds::RequestedIncompatibleQosStatus& status) override
        {
            (void)datareader;
            reader_.incompatible_qos(status);
        }

        void on_liveliness_changed(
                eprosima::fastdds::dds::DataReader* datareader,
                const eprosima::fastrtps::LivelinessChangedStatus& status) override
        {
            (void)datareader;

            reader_.set_liveliness_changed_status(status);

            if (status.alive_count_change == 1)
            {
                reader_.liveliness_recovered();

            }
            else if (status.not_alive_count_change == 1)
            {
                reader_.liveliness_lost();

            }
        }

        unsigned int missed_deadlines() const
        {
            return times_deadline_missed_;
        }

    private:

        Listener& operator =(
                const Listener&) = delete;

        PubSubReader& reader_;

        //! Number of times deadline was missed
        unsigned int times_deadline_missed_;

    }
    listener_;

    friend class Listener;

public:

    PubSubReader(
            const std::string& topic_name,
            bool take = true,
            bool statistics = false)
        : participant_listener_(*this)
        , listener_(*this)
        , participant_(nullptr)
        , topic_(nullptr)
        , subscriber_(nullptr)
        , datareader_(nullptr)
        , status_mask_(eprosima::fastdds::dds::StatusMask::all())
        , topic_name_(topic_name)
        , initialized_(false)
        , matched_(0)
        , participant_matched_(0)
        , receiving_(false)
        , current_processed_count_(0)
        , number_samples_expected_(0)
        , discovery_result_(false)
        , onDiscovery_(nullptr)
        , onEndpointDiscovery_(nullptr)
        , take_(take)
        , statistics_(statistics)
#if HAVE_SECURITY
        , authorized_(0)
        , unauthorized_(0)
#endif // if HAVE_SECURITY
        , liveliness_mutex_()
        , liveliness_cv_()
        , times_liveliness_lost_(0)
        , times_liveliness_recovered_(0)
        , times_incompatible_qos_(0)
        , last_incompatible_qos_(eprosima::fastdds::dds::INVALID_QOS_POLICY_ID)
        , message_receive_count_(0)
    {
        // Generate topic name
        if (!statistics)
        {
            std::ostringstream t;
            t << topic_name_ << "_" << asio::ip::host_name() << "_" << GET_PID();
            topic_name_ = t.str();
        }

        if (enable_datasharing)
        {
            datareader_qos_.data_sharing().automatic();
        }
        else
        {
            datareader_qos_.data_sharing().off();
        }

        // By default, memory mode is preallocated (the most restritive)
        datareader_qos_.endpoint().history_memory_policy = eprosima::fastrtps::rtps::PREALLOCATED_MEMORY_MODE;

        // By default, heartbeat period delay is 100 milliseconds.
        datareader_qos_.reliable_reader_qos().times.heartbeatResponseDelay.seconds = 0;
        datareader_qos_.reliable_reader_qos().times.heartbeatResponseDelay.nanosec = 100000000;
    }

    virtual ~PubSubReader()
    {
        destroy();
    }

    eprosima::fastdds::dds::DataReader& get_native_reader() const
    {
        return *datareader_;
    }

    void init()
    {
        ASSERT_FALSE(initialized_);
        matched_ = 0;

        if (!xml_file_.empty())
        {
            DomainParticipantFactory::get_instance()->load_XML_profiles_file(xml_file_);
            if (!participant_profile_.empty())
            {
                participant_ = DomainParticipantFactory::get_instance()->create_participant_with_profile(
                    (uint32_t)GET_PID() % 230,
                    participant_profile_,
                    &participant_listener_,
                    eprosima::fastdds::dds::StatusMask::none());
                ASSERT_NE(participant_, nullptr);
                ASSERT_TRUE(participant_->is_enabled());
            }
        }
        if (participant_ == nullptr)
        {
            participant_ = DomainParticipantFactory::get_instance()->create_participant(
                (uint32_t)GET_PID() % 230,
                participant_qos_,
                &participant_listener_,
                eprosima::fastdds::dds::StatusMask::none());
            ASSERT_NE(participant_, nullptr);
            ASSERT_TRUE(participant_->is_enabled());
        }

        participant_guid_ = participant_->guid();

        type_.reset(new type_support());

        // Register type
        ASSERT_EQ(participant_->register_type(type_), ReturnCode_t::RETCODE_OK);

        // Create topic
        topic_ =
                participant_->create_topic(topic_name_, type_->getName(),
                        eprosima::fastdds::dds::TOPIC_QOS_DEFAULT);
        ASSERT_NE(topic_, nullptr);
        ASSERT_TRUE(topic_->is_enabled());

        // Create publisher
        createSubscriber();
    }

    virtual void createSubscriber()
    {
        if (participant_ != nullptr)
        {
            subscriber_ = participant_->create_subscriber(subscriber_qos_);
            ASSERT_NE(subscriber_, nullptr);
            ASSERT_TRUE(subscriber_->is_enabled());

            if (!xml_file_.empty())
            {
                if (!datareader_profile_.empty())
                {
                    datareader_ = subscriber_->create_datareader_with_profile(topic_, datareader_profile_, &listener_,
                                    status_mask_);
                    ASSERT_NE(datareader_, nullptr);
                    ASSERT_TRUE(datareader_->is_enabled());
                }
            }
            if (datareader_ == nullptr)
            {
                datareader_ = subscriber_->create_datareader(topic_, datareader_qos_, &listener_, status_mask_);
            }

            if (datareader_ != nullptr)
            {
                std::cout << "Created datareader " << datareader_->guid() << " for topic " <<
                    topic_name_ << std::endl;
                initialized_ = true;
                datareader_guid_ = datareader_->guid();
            }
        }

    }

    bool isInitialized() const
    {
        return initialized_;
    }

    virtual void destroy()
    {
        if (participant_ != nullptr)
        {
            if (datareader_)
            {
                subscriber_->delete_datareader(datareader_);
                datareader_ = nullptr;
            }
            if (subscriber_)
            {
                participant_->delete_subscriber(subscriber_);
                subscriber_ = nullptr;
            }
            if (topic_)
            {
                participant_->delete_topic(topic_);
                topic_ = nullptr;
            }
            eprosima::fastdds::dds::DomainParticipantFactory::get_instance()->delete_participant(participant_);
            participant_ = nullptr;
        }

        initialized_ = false;
    }

    std::list<type> data_not_received()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return total_msgs_;
    }

    eprosima::fastrtps::rtps::SequenceNumber_t startReception(
            std::list<type>& msgs)
    {
        mutex_.lock();
        total_msgs_ = msgs;
        number_samples_expected_ = total_msgs_.size();
        current_processed_count_ = 0;
        last_seq.clear();
        mutex_.unlock();

        bool ret = false;
        do
        {
            receive_one(datareader_, ret);
        }
        while (ret);

        receiving_.store(true);
        return get_last_sequence_received();
    }

    void stopReception()
    {
        receiving_.store(false);
    }

    template<class _Rep,
            class _Period
            >
    void wait_for_all_received(
            const std::chrono::duration<_Rep, _Period>& max_wait,
            size_t num_messages = 0)
    {
        if (num_messages == 0)
        {
            num_messages = number_samples_expected_;
        }
        std::unique_lock<std::mutex> lock(message_receive_mutex_);
        message_receive_cv_.wait_for(lock, max_wait, [this, num_messages]() -> bool
                {
                    return num_messages == message_receive_count_;
                });
    }

    void block_for_all()
    {
        block([this]() -> bool
                {
                    return number_samples_expected_ == current_processed_count_;
                });
    }

    void block_for_seq(
            eprosima::fastrtps::rtps::SequenceNumber_t seq)
    {
        block([this, seq]() -> bool
                {
                    return get_last_sequence_received() == seq;
                });
    }

    size_t block_for_at_least(
            size_t at_least)
    {
        block([this, at_least]() -> bool
                {
                    return current_processed_count_ >= at_least;
                });
        return current_processed_count_;
    }

    void block(
            std::function<bool()> checker)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, checker);
    }

    template<class _Rep,
            class _Period
            >
    size_t block_for_all(
            const std::chrono::duration<_Rep, _Period>& max_wait)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, max_wait, [this]() -> bool
                {
                    return number_samples_expected_ == current_processed_count_;
                });

        return current_processed_count_;
    }

    void check_history_content(
            std::list<type>& expected_messages)
    {
        FASTDDS_SEQUENCE(DataSeq, type);
        DataSeq data_seq;
        eprosima::fastdds::dds::SampleInfoSeq info_seq;

        ReturnCode_t success =
                datareader_->read(data_seq, info_seq,
                        eprosima::fastdds::dds::LENGTH_UNLIMITED,
                        eprosima::fastdds::dds::ANY_SAMPLE_STATE,
                        eprosima::fastdds::dds::ANY_VIEW_STATE,
                        eprosima::fastdds::dds::ANY_INSTANCE_STATE);

        if (ReturnCode_t::RETCODE_OK == success)
        {
            for (eprosima::fastdds::dds::LoanableCollection::size_type n = 0; n < info_seq.length(); ++n)
            {
                if (info_seq[n].valid_data)
                {
                    auto it = std::find(expected_messages.begin(), expected_messages.end(), data_seq[n]);
                    ASSERT_NE(it, expected_messages.end());
                    expected_messages.erase(it);
                }
            }
            ASSERT_TRUE(expected_messages.empty());
            datareader_->return_loan(data_seq, info_seq);
        }
    }

    void wait_discovery(
            std::chrono::seconds timeout = std::chrono::seconds::zero(),
            unsigned int min_writers = 1)
    {
        std::unique_lock<std::mutex> lock(mutexDiscovery_);

        std::cout << "Reader is waiting discovery..." << std::endl;

        if (timeout == std::chrono::seconds::zero())
        {
            cvDiscovery_.wait(lock, [&]()
                    {
                        return matched_ >= min_writers;
                    });
        }
        else
        {
            cvDiscovery_.wait_for(lock, timeout, [&]()
                    {
                        return matched_ >= min_writers;
                    });
        }

        std::cout << "Reader discovery finished..." << std::endl;
    }

    bool wait_participant_discovery(
            unsigned int min_participants = 1,
            std::chrono::seconds timeout = std::chrono::seconds::zero())
    {
        bool ret_value = true;
        std::unique_lock<std::mutex> lock(mutexDiscovery_);

        std::cout << "Reader is waiting discovery of at least " << min_participants << " participants..." << std::endl;

        if (timeout == std::chrono::seconds::zero())
        {
            cvDiscovery_.wait(lock, [&]()
                    {
                        return participant_matched_ >= min_participants;
                    });
        }
        else
        {
            if (!cvDiscovery_.wait_for(lock, timeout, [&]()
                    {
                        return participant_matched_ >= min_participants;
                    }))
            {
                ret_value = false;
            }
        }

        if (ret_value)
        {
            std::cout << "Reader participant discovery finished successfully..." << std::endl;
        }
        else
        {
            std::cout << "Reader participant discovery finished unsuccessfully..." << std::endl;
        }

        return ret_value;
    }

    bool wait_participant_undiscovery(
            std::chrono::seconds timeout = std::chrono::seconds::zero())
    {
        bool ret_value = true;
        std::unique_lock<std::mutex> lock(mutexDiscovery_);

        std::cout << "Reader is waiting participant undiscovery..." << std::endl;

        if (timeout == std::chrono::seconds::zero())
        {
            cvDiscovery_.wait(lock, [&]()
                    {
                        return participant_matched_ == 0;
                    });
        }
        else
        {
            if (!cvDiscovery_.wait_for(lock, timeout, [&]()
                    {
                        return participant_matched_ == 0;
                    }))
            {
                ret_value = false;
            }
        }

        if (ret_value)
        {
            std::cout << "Reader participant undiscovery finished successfully..." << std::endl;
        }
        else
        {
            std::cout << "Reader participant undiscovery finished unsuccessfully..." << std::endl;
        }

        return ret_value;
    }

    void wait_writer_undiscovery()
    {
        std::unique_lock<std::mutex> lock(mutexDiscovery_);

        std::cout << "Reader is waiting removal..." << std::endl;

        cvDiscovery_.wait(lock, [&]()
                {
                    return matched_ == 0;
                });

        std::cout << "Reader removal finished..." << std::endl;
    }

    void wait_liveliness_recovered(
            unsigned int times = 1)
    {
        std::unique_lock<std::mutex> lock(liveliness_mutex_);

        liveliness_cv_.wait(lock, [&]()
                {
                    return times_liveliness_recovered_ >= times;
                });
    }

    void wait_liveliness_lost(
            unsigned int times = 1)
    {
        std::unique_lock<std::mutex> lock(liveliness_mutex_);

        liveliness_cv_.wait(lock, [&]()
                {
                    return times_liveliness_lost_ >= times;
                });
    }

    void wait_incompatible_qos(
            unsigned int times = 1)
    {
        std::unique_lock<std::mutex> lock(incompatible_qos_mutex_);

        incompatible_qos_cv_.wait(lock, [&]()
                {
                    return times_incompatible_qos_ >= times;
                });
    }

    void incompatible_qos(
            eprosima::fastdds::dds::OfferedIncompatibleQosStatus status)
    {
        std::unique_lock<std::mutex> lock(incompatible_qos_mutex_);
        times_incompatible_qos_++;
        last_incompatible_qos_ = status.last_policy_id;
        incompatible_qos_cv_.notify_one();
    }

#if HAVE_SECURITY
    void waitAuthorized()
    {
        std::unique_lock<std::mutex> lock(mutexAuthentication_);

        std::cout << "Reader is waiting authorization..." << std::endl;

        cvAuthentication_.wait(lock, [&]() -> bool
                {
                    return authorized_ > 0;
                });

        std::cout << "Reader authorization finished..." << std::endl;
    }

    void waitUnauthorized()
    {
        std::unique_lock<std::mutex> lock(mutexAuthentication_);

        std::cout << "Reader is waiting unauthorization..." << std::endl;

        cvAuthentication_.wait(lock, [&]() -> bool
                {
                    return unauthorized_ > 0;
                });

        std::cout << "Reader unauthorization finished..." << std::endl;
    }

#endif // if HAVE_SECURITY

    size_t getReceivedCount() const
    {
        return current_processed_count_;
    }

    eprosima::fastrtps::rtps::SequenceNumber_t get_last_sequence_received()
    {
        if (last_seq.empty())
        {
            return eprosima::fastrtps::rtps::SequenceNumber_t();
        }

        using pair_type = typename decltype(last_seq)::value_type;
        auto seq_comp = [](const pair_type& v1, const pair_type& v2) -> bool
                {
                    return v1.second < v2.second;
                };
        return std::max_element(last_seq.cbegin(), last_seq.cend(), seq_comp)->second;
    }

    PubSubReader& deactivate_status_listener(
            eprosima::fastdds::dds::StatusMask mask)
    {
        status_mask_ &= ~mask;
        return *this;
    }

    PubSubReader& activate_status_listener(
            eprosima::fastdds::dds::StatusMask mask)
    {
        status_mask_ |= mask;
        return *this;
    }

    PubSubReader& reset_status_listener()
    {
        status_mask_ = eprosima::fastdds::dds::StatusMask::all();
        return *this;
    }

    /*** Function to change QoS ***/
    PubSubReader& reliability(
            const eprosima::fastrtps::ReliabilityQosPolicyKind kind)
    {
        datareader_qos_.reliability().kind = kind;
        return *this;
    }

    PubSubReader& mem_policy(
            const eprosima::fastrtps::rtps::MemoryManagementPolicy mem_policy)
    {
        datareader_qos_.endpoint().history_memory_policy = mem_policy;
        return *this;
    }

    PubSubReader& deadline_period(
            const eprosima::fastrtps::Duration_t deadline_period)
    {
        datareader_qos_.deadline().period = deadline_period;
        return *this;
    }

    bool update_deadline_period(
            const eprosima::fastrtps::Duration_t& deadline_period)
    {
        eprosima::fastdds::dds::DataReaderQos datareader_qos;
        datareader_->get_qos(datareader_qos);
        datareader_qos.deadline().period = deadline_period;

        return (datareader_->set_qos(datareader_qos) == ReturnCode_t::RETCODE_OK);
    }

    PubSubReader& liveliness_kind(
            const eprosima::fastrtps::LivelinessQosPolicyKind& kind)
    {
        datareader_qos_.liveliness().kind = kind;
        return *this;
    }

    PubSubReader& liveliness_lease_duration(
            const eprosima::fastrtps::Duration_t lease_duration)
    {
        datareader_qos_.liveliness().lease_duration = lease_duration;
        return *this;
    }

    PubSubReader& latency_budget_duration(
            const eprosima::fastrtps::Duration_t& latency_duration)
    {
        datareader_qos_.latency_budget().duration = latency_duration;
        return *this;
    }

    eprosima::fastrtps::Duration_t get_latency_budget_duration()
    {
        return datareader_qos_.latency_budget().duration;
    }

    PubSubReader& lifespan_period(
            const eprosima::fastrtps::Duration_t lifespan_period)
    {
        datareader_qos_.lifespan().duration = lifespan_period;
        return *this;
    }

    PubSubReader& keep_duration(
            const eprosima::fastrtps::Duration_t duration)
    {
        datareader_qos_.reliable_reader_qos().disable_positive_ACKs.enabled = true;
        datareader_qos_.reliable_reader_qos().disable_positive_ACKs.duration = duration;
        return *this;
    }

    PubSubReader& history_kind(
            const eprosima::fastrtps::HistoryQosPolicyKind kind)
    {
        datareader_qos_.history().kind = kind;
        return *this;
    }

    PubSubReader& history_depth(
            const int32_t depth)
    {
        datareader_qos_.history().depth = depth;
        return *this;
    }

    PubSubReader& disable_builtin_transport()
    {
        participant_qos_.transport().use_builtin_transports = false;
        return *this;
    }

    PubSubReader& add_user_transport_to_pparams(
            std::shared_ptr<eprosima::fastdds::rtps::TransportDescriptorInterface> userTransportDescriptor)
    {
        participant_qos_.transport().user_transports.push_back(userTransportDescriptor);
        return *this;
    }

    PubSubReader& resource_limits_allocated_samples(
            const int32_t initial)
    {
        datareader_qos_.resource_limits().allocated_samples = initial;
        return *this;
    }

    PubSubReader& resource_limits_max_samples(
            const int32_t max)
    {
        datareader_qos_.resource_limits().max_samples = max;
        return *this;
    }

    PubSubReader& resource_limits_max_instances(
            const int32_t max)
    {
        datareader_qos_.resource_limits().max_instances = max;
        return *this;
    }

    PubSubReader& resource_limits_max_samples_per_instance(
            const int32_t max)
    {
        datareader_qos_.resource_limits().max_samples_per_instance = max;
        return *this;
    }

    PubSubReader& matched_writers_allocation(
            size_t initial,
            size_t maximum)
    {
        datareader_qos_.reader_resource_limits().matched_publisher_allocation.initial = initial;
        datareader_qos_.reader_resource_limits().matched_publisher_allocation.maximum = maximum;
        return *this;
    }

    PubSubReader& expect_no_allocs()
    {
        // TODO(Mcc): Add no allocations check code when feature is completely ready
        return *this;
    }

    PubSubReader& heartbeatResponseDelay(
            const int32_t secs,
            const int32_t frac)
    {
        datareader_qos_.reliable_reader_qos().times.heartbeatResponseDelay.seconds = secs;
        datareader_qos_.reliable_reader_qos().times.heartbeatResponseDelay.fraction(frac);
        return *this;
    }

    PubSubReader& unicastLocatorList(
            const eprosima::fastdds::rtps::LocatorList& unicast_locators)
    {
        datareader_qos_.endpoint().unicast_locator_list = unicast_locators;
        return *this;
    }

    PubSubReader& add_to_unicast_locator_list(
            const std::string& ip,
            uint32_t port)
    {
        eprosima::fastdds::rtps::Locator loc;
        if (!IPLocator::setIPv4(loc, ip))
        {
            loc.kind = LOCATOR_KIND_UDPv6;
            if (!IPLocator::setIPv6(loc, ip))
            {
                return *this;
            }
        }
        loc.port = port;
        datareader_qos_.endpoint().unicast_locator_list.push_back(loc);

        return *this;
    }

    PubSubReader& multicastLocatorList(
            const eprosima::fastdds::rtps::LocatorList& multicast_locators)
    {
        datareader_qos_.endpoint().multicast_locator_list = multicast_locators;
        return *this;
    }

    PubSubReader& add_to_multicast_locator_list(
            const std::string& ip,
            uint32_t port)
    {
        eprosima::fastdds::rtps::Locator loc;
        if (!IPLocator::setIPv4(loc, ip))
        {
            loc.kind = LOCATOR_KIND_UDPv6;
            if (!IPLocator::setIPv6(loc, ip))
            {
                return *this;
            }
        }

        loc.port = port;
        datareader_qos_.endpoint().multicast_locator_list.push_back(loc);

        return *this;
    }

    PubSubReader& metatraffic_unicast_locator_list(
            const eprosima::fastdds::rtps::LocatorList& unicast_locators)
    {
        participant_qos_.wire_protocol().builtin.metatrafficUnicastLocatorList = unicast_locators;
        return *this;
    }

    PubSubReader& add_to_metatraffic_unicast_locator_list(
            const std::string& ip,
            uint32_t port)
    {
        eprosima::fastdds::rtps::Locator loc;
        if (!IPLocator::setIPv4(loc, ip))
        {
            loc.kind = LOCATOR_KIND_UDPv6;
            if (!IPLocator::setIPv6(loc, ip))
            {
                return *this;
            }
        }

        loc.port = port;
        participant_qos_.wire_protocol().builtin.metatrafficUnicastLocatorList.push_back(loc);

        return *this;
    }

    PubSubReader& metatraffic_multicast_locator_list(
            const eprosima::fastdds::rtps::LocatorList& multicast_locators)
    {
        participant_qos_.wire_protocol().builtin.metatrafficMulticastLocatorList = multicast_locators;
        return *this;
    }

    PubSubReader& add_to_metatraffic_multicast_locator_list(
            const std::string& ip,
            uint32_t port)
    {
        eprosima::fastdds::rtps::Locator loc;
        if (!IPLocator::setIPv4(loc, ip))
        {
            loc.kind = LOCATOR_KIND_UDPv6;
            if (!IPLocator::setIPv6(loc, ip))
            {
                return *this;
            }
        }

        loc.port = port;
        participant_qos_.wire_protocol().builtin.metatrafficMulticastLocatorList.push_back(loc);

        return *this;
    }

    PubSubReader& set_default_unicast_locators(
            const eprosima::fastdds::rtps::LocatorList& locators)
    {
        participant_qos_.wire_protocol().default_unicast_locator_list = locators;
        return *this;
    }

    PubSubReader& add_to_default_unicast_locator_list(
            const std::string& ip,
            uint32_t port)
    {
        eprosima::fastdds::rtps::Locator loc;
        if (!IPLocator::setIPv4(loc, ip))
        {
            loc.kind = LOCATOR_KIND_UDPv6;
            if (!IPLocator::setIPv6(loc, ip))
            {
                return *this;
            }
        }

        loc.port = port;
        participant_qos_.wire_protocol().default_unicast_locator_list.push_back(loc);

        return *this;
    }

    PubSubReader& set_default_multicast_locators(
            const eprosima::fastdds::rtps::LocatorList& locators)
    {
        participant_qos_.wire_protocol().default_multicast_locator_list = locators;
        return *this;
    }

    PubSubReader& add_to_default_multicast_locator_list(
            const std::string& ip,
            uint32_t port)
    {
        eprosima::fastdds::rtps::Locator loc;
        if (!IPLocator::setIPv4(loc, ip))
        {
            loc.kind = LOCATOR_KIND_UDPv6;
            if (!IPLocator::setIPv6(loc, ip))
            {
                return *this;
            }
        }

        loc.port = port;
        participant_qos_.wire_protocol().default_multicast_locator_list.push_back(loc);

        return *this;
    }

    PubSubReader& initial_peers(
            const eprosima::fastdds::rtps::LocatorList& initial_peers)
    {
        participant_qos_.wire_protocol().builtin.initialPeersList = initial_peers;
        return *this;
    }

    PubSubReader& ignore_participant_flags(
            eprosima::fastrtps::rtps::ParticipantFilteringFlags_t flags)
    {
        participant_qos_.wire_protocol().builtin.discovery_config.ignoreParticipantFlags = flags;
        return *this;
    }

    PubSubReader& socket_buffer_size(
            uint32_t sockerBufferSize)
    {
        participant_qos_.transport().listen_socket_buffer_size = sockerBufferSize;
        return *this;
    }

    PubSubReader& durability_kind(
            const eprosima::fastrtps::DurabilityQosPolicyKind kind)
    {
        datareader_qos_.durability().kind = kind;
        return *this;
    }

    PubSubReader& static_discovery(
            const char* filename)
    {
        participant_qos_.wire_protocol().builtin.discovery_config.use_SIMPLE_EndpointDiscoveryProtocol = false;
        participant_qos_.wire_protocol().builtin.discovery_config.use_STATIC_EndpointDiscoveryProtocol = true;
        participant_qos_.wire_protocol().builtin.discovery_config.static_edp_xml_config(filename);
        return *this;
    }

    PubSubReader& setSubscriberIDs(
            uint8_t UserID,
            uint8_t EntityID)
    {
        datareader_qos_.endpoint().user_defined_id = UserID;
        datareader_qos_.endpoint().entity_id = EntityID;
        return *this;

    }

    PubSubReader& setManualTopicName(
            std::string topic_name)
    {
        topic_name_ = topic_name;
        return *this;
    }

    PubSubReader& disable_multicast(
            int32_t participantId)
    {
        participant_qos_.wire_protocol().participant_id = participantId;

        eprosima::fastdds::rtps::LocatorList default_unicast_locators;
        eprosima::fastdds::rtps::Locator default_unicast_locator;
        eprosima::fastdds::rtps::Locator loopback_locator;
        if (!use_udpv4)
        {
            default_unicast_locator.kind = LOCATOR_KIND_UDPv6;
            loopback_locator.kind = LOCATOR_KIND_UDPv6;
        }

        default_unicast_locators.push_back(default_unicast_locator);
        participant_qos_.wire_protocol().builtin.metatrafficUnicastLocatorList = default_unicast_locators;

        if (!IPLocator::setIPv4(loopback_locator, 127, 0, 0, 1))
        {
            IPLocator::setIPv6(loopback_locator, "::1");
        }
        participant_qos_.wire_protocol().builtin.initialPeersList.push_back(loopback_locator);
        return *this;
    }

    PubSubReader& property_policy(
            const eprosima::fastrtps::rtps::PropertyPolicy& property_policy)
    {
        participant_qos_.properties() = property_policy;
        return *this;
    }

    PubSubReader& entity_property_policy(
            const eprosima::fastrtps::rtps::PropertyPolicy& property_policy)
    {
        datareader_qos_.properties() = property_policy;
        return *this;
    }

    PubSubReader& partition(
            const std::string& partition)
    {
        subscriber_qos_.partition().push_back(partition.c_str());
        return *this;
    }

    PubSubReader& userData(
            std::vector<eprosima::fastrtps::rtps::octet> user_data)
    {
        participant_qos_.user_data() = user_data;
        return *this;
    }

    PubSubReader& user_data_max_size(
            size_t max_user_data)
    {
        participant_qos_.allocation().data_limits.max_user_data = max_user_data;
        return *this;
    }

    PubSubReader& properties_max_size(
            size_t max_properties)
    {
        participant_qos_.allocation().data_limits.max_properties = max_properties;
        return *this;
    }

    PubSubReader& partitions_max_size(
            size_t max_partitions)
    {
        participant_qos_.allocation().data_limits.max_partitions = max_partitions;
        return *this;
    }

    PubSubReader& lease_duration(
            eprosima::fastrtps::Duration_t lease_duration,
            eprosima::fastrtps::Duration_t announce_period)
    {
        participant_qos_.wire_protocol().builtin.discovery_config.leaseDuration = lease_duration;
        participant_qos_.wire_protocol().builtin.discovery_config.leaseDuration_announcementperiod = announce_period;
        return *this;
    }

    PubSubReader& load_participant_attr(
            const std::string& /*xml*/)
    {
        /* TODO
           std::unique_ptr<eprosima::fastrtps::xmlparser::BaseNode> root;
           if (eprosima::fastrtps::xmlparser::XMLParser::loadXML(xml.data(), xml.size(),
                root) == eprosima::fastrtps::xmlparser::XMLP_ret::XML_OK)
           {
            for (const auto& profile : root->getChildren())
            {
                if (profile->getType() == eprosima::fastrtps::xmlparser::NodeType::PARTICIPANT)
                {
                    participant_attr_ =
         *(dynamic_cast<eprosima::fastrtps::xmlparser::DataNode<eprosima::fastrtps::ParticipantAttributes>
         *>(
                                profile.get())->get());
                }
            }
           }
         */
        return *this;
    }

    PubSubReader& load_subscriber_attr(
            const std::string& /*xml*/)
    {
        /* TODO
           std::unique_ptr<eprosima::fastrtps::xmlparser::BaseNode> root;
           if (eprosima::fastrtps::xmlparser::XMLParser::loadXML(xml.data(), xml.size(),
                root) == eprosima::fastrtps::xmlparser::XMLP_ret::XML_OK)
           {
            for (const auto& profile : root->getChildren())
            {
                if (profile->getType() == eprosima::fastrtps::xmlparser::NodeType::SUBSCRIBER)
                {
                    subscriber_attr_ =
         *(dynamic_cast<eprosima::fastrtps::xmlparser::DataNode<eprosima::fastrtps::SubscriberAttributes>
         *>(
                                profile.get())->get());
                }
            }
           }
         */
        return *this;
    }

    PubSubReader& max_initial_peers_range(
            uint32_t maxInitialPeerRange)
    {
        participant_qos_.transport().use_builtin_transports = false;
        std::shared_ptr<UDPTransportDescriptor> descriptor;
        if (use_udpv4)
        {
            descriptor = std::make_shared<UDPv4TransportDescriptor>();
        }
        else
        {
            descriptor = std::make_shared<UDPv6TransportDescriptor>();
        }
        descriptor->maxInitialPeersRange = maxInitialPeerRange;
        participant_qos_.transport().user_transports.push_back(descriptor);
        return *this;
    }

    PubSubReader& participant_id(
            int32_t participantId)
    {
        participant_qos_.wire_protocol().participant_id = participantId;
        return *this;
    }

    PubSubReader& datasharing_off()
    {
        datareader_qos_.data_sharing().off();
        return *this;
    }

    PubSubReader& datasharing_auto(
            std::vector<uint16_t> domain_id = std::vector<uint16_t>())
    {
        datareader_qos_.data_sharing().automatic(domain_id);
        return *this;
    }

    PubSubReader& datasharing_auto(
            const std::string directory,
            std::vector<uint16_t> domain_id = std::vector<uint16_t>())
    {
        datareader_qos_.data_sharing().automatic(directory, domain_id);
        return *this;
    }

    PubSubReader& datasharing_on(
            const std::string directory,
            std::vector<uint16_t> domain_id = std::vector<uint16_t>())
    {
        datareader_qos_.data_sharing().on(directory, domain_id);
        return *this;
    }

#if HAVE_SQLITE3
    PubSubReader& make_persistent(
            const std::string& filename,
            const std::string& persistence_guid)
    {
        participant_qos_.properties().properties().emplace_back("dds.persistence.plugin", "builtin.SQLITE3");
        participant_qos_.properties().properties().emplace_back("dds.persistence.sqlite3.filename", filename);
        datareader_qos_.durability().kind = eprosima::fastrtps::TRANSIENT_DURABILITY_QOS;
        datareader_qos_.properties().properties().emplace_back("dds.persistence.guid", persistence_guid);

        return *this;
    }

#endif // if HAVE_SQLITE3

    bool update_partition(
            const std::string& partition)
    {
        subscriber_qos_.partition().clear();
        subscriber_qos_.partition().push_back(partition.c_str());
        return (ReturnCode_t::RETCODE_OK == subscriber_->set_qos(subscriber_qos_));
    }

    bool clear_partitions()
    {
        subscriber_qos_.partition().clear();
        return (ReturnCode_t::RETCODE_OK == subscriber_->set_qos(subscriber_qos_));
    }

    /*** Function for discovery callback ***/

    void wait_discovery_result()
    {
        std::unique_lock<std::mutex> lock(mutexDiscovery_);

        std::cout << "Reader is waiting discovery result..." << std::endl;

        cvDiscovery_.wait(lock, [&]()
                {
                    return discovery_result_;
                });

        std::cout << "Reader gets discovery result..." << std::endl;
    }

    void setOnDiscoveryFunction(
            std::function<bool(const eprosima::fastrtps::rtps::ParticipantDiscoveryInfo&)> f)
    {
        onDiscovery_ = f;
    }

    void setOnEndpointDiscoveryFunction(
            std::function<bool(const eprosima::fastrtps::rtps::WriterDiscoveryInfo&)> f)
    {
        onEndpointDiscovery_ = f;
    }

    bool take_first_data(
            void* data)
    {
        using collection = eprosima::fastdds::dds::UserAllocatedSequence;
        using info_seq_type = eprosima::fastdds::dds::SampleInfoSeq;

        collection::element_type buf[1] = { data };
        collection data_seq(buf, 1);
        info_seq_type info_seq(1);

        if (ReturnCode_t::RETCODE_OK == datareader_->take(data_seq, info_seq))
        {
            current_processed_count_++;
            return true;
        }
        return false;
    }

    bool takeNextData(
            void* data)
    {
        eprosima::fastdds::dds::SampleInfo dds_info;
        if (datareader_->take_next_sample(data, &dds_info) == ReturnCode_t::RETCODE_OK)
        {
            current_processed_count_++;
            return true;
        }
        return false;
    }

    unsigned int missed_deadlines() const
    {
        return listener_.missed_deadlines();
    }

    void liveliness_lost()
    {
        std::unique_lock<std::mutex> lock(liveliness_mutex_);
        times_liveliness_lost_++;
        liveliness_cv_.notify_one();
    }

    void liveliness_recovered()
    {
        std::unique_lock<std::mutex> lock(liveliness_mutex_);
        times_liveliness_recovered_++;
        liveliness_cv_.notify_one();
    }

    void set_liveliness_changed_status(
            const eprosima::fastrtps::LivelinessChangedStatus& status)
    {
        std::unique_lock<std::mutex> lock(liveliness_mutex_);

        liveliness_changed_status_ = status;
    }

    unsigned int times_liveliness_lost()
    {
        std::unique_lock<std::mutex> lock(liveliness_mutex_);

        return times_liveliness_lost_;
    }

    unsigned int times_liveliness_recovered()
    {
        std::unique_lock<std::mutex> lock(liveliness_mutex_);

        return times_liveliness_recovered_;
    }

    unsigned int times_incompatible_qos()
    {
        std::unique_lock<std::mutex> lock(incompatible_qos_mutex_);

        return times_incompatible_qos_;
    }

    eprosima::fastdds::dds::QosPolicyId_t last_incompatible_qos()
    {
        std::unique_lock<std::mutex> lock(incompatible_qos_mutex_);

        return last_incompatible_qos_;
    }

    eprosima::fastdds::dds::RequestedIncompatibleQosStatus get_incompatible_qos_status() const
    {
        eprosima::fastdds::dds::RequestedIncompatibleQosStatus status;
        datareader_->get_requested_incompatible_qos_status(status);
        return status;
    }

    const eprosima::fastrtps::LivelinessChangedStatus& liveliness_changed_status()
    {
        std::unique_lock<std::mutex> lock(liveliness_mutex_);

        return liveliness_changed_status_;
    }

    eprosima::fastdds::dds::LivelinessChangedStatus get_liveliness_changed_status() const
    {
        eprosima::fastdds::dds::LivelinessChangedStatus status;
        datareader_->get_liveliness_changed_status(status);
        return status;
    }

    bool is_matched() const
    {
        return matched_ > 0;
    }

    void set_xml_filename(
            const std::string& name)
    {
        xml_file_ = name;
    }

    void set_participant_profile(
            const std::string& profile)
    {
        participant_profile_ = profile;
    }

    void set_datareader_profile(
            const std::string& profile)
    {
        datareader_profile_ = profile;
    }

    eprosima::fastdds::dds::StatusCondition& get_statuscondition() const
    {
        return datareader_->get_statuscondition();
    }

    const eprosima::fastrtps::rtps::GUID_t& datareader_guid() const
    {
        return datareader_guid_;
    }

protected:

    const eprosima::fastrtps::rtps::GUID_t& participant_guid() const
    {
        return participant_guid_;
    }

    void receive_one(
            eprosima::fastdds::dds::DataReader* datareader,
            bool& returnedValue)
    {
        returnedValue = false;
        type data;
        eprosima::fastdds::dds::SampleInfo info;

        ReturnCode_t success = take_ ?
                datareader->take_next_sample((void*)&data, &info) :
                datareader->read_next_sample((void*)&data, &info);
        if (ReturnCode_t::RETCODE_OK == success)
        {
            returnedValue = true;

            std::unique_lock<std::mutex> lock(mutex_);

            // Check order of changes.
            ASSERT_LT(last_seq[info.instance_handle], info.sample_identity.sequence_number());
            last_seq[info.instance_handle] = info.sample_identity.sequence_number();

            if (info.instance_state == eprosima::fastdds::dds::ALIVE_INSTANCE_STATE)
            {
                auto it = std::find(total_msgs_.begin(), total_msgs_.end(), data);
                ASSERT_NE(it, total_msgs_.end());
                total_msgs_.erase(it);
                ++current_processed_count_;
                default_receive_print<type>(data);
                cv_.notify_one();
            }
        }
    }

    void participant_matched()
    {
        std::unique_lock<std::mutex> lock(mutexDiscovery_);
        ++participant_matched_;
        cvDiscovery_.notify_one();
    }

    void participant_unmatched()
    {
        std::unique_lock<std::mutex> lock(mutexDiscovery_);
        --participant_matched_;
        cvDiscovery_.notify_one();
    }

    void matched()
    {
        std::unique_lock<std::mutex> lock(mutexDiscovery_);
        ++matched_;
        cvDiscovery_.notify_one();
    }

    void unmatched()
    {
        std::unique_lock<std::mutex> lock(mutexDiscovery_);
        --matched_;
        cvDiscovery_.notify_one();
    }

#if HAVE_SECURITY
    void authorized()
    {
        mutexAuthentication_.lock();
        ++authorized_;
        mutexAuthentication_.unlock();
        cvAuthentication_.notify_all();
    }

    void unauthorized()
    {
        mutexAuthentication_.lock();
        ++unauthorized_;
        mutexAuthentication_.unlock();
        cvAuthentication_.notify_all();
    }

#endif // if HAVE_SECURITY

    PubSubReader& operator =(
            const PubSubReader&) = delete;

    eprosima::fastdds::dds::DomainParticipant* participant_;
    eprosima::fastdds::dds::DomainParticipantQos participant_qos_;
    eprosima::fastdds::dds::Topic* topic_;
    eprosima::fastdds::dds::Subscriber* subscriber_;
    eprosima::fastdds::dds::SubscriberQos subscriber_qos_;
    eprosima::fastdds::dds::DataReader* datareader_;
    eprosima::fastdds::dds::DataReaderQos datareader_qos_;
    eprosima::fastdds::dds::StatusMask status_mask_;
    std::string topic_name_;
    eprosima::fastrtps::rtps::GUID_t participant_guid_;
    eprosima::fastrtps::rtps::GUID_t datareader_guid_;
    bool initialized_;
    std::list<type> total_msgs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::mutex mutexDiscovery_;
    std::condition_variable cvDiscovery_;
    std::atomic<unsigned int> matched_;
    unsigned int participant_matched_;
    std::atomic<bool> receiving_;
    eprosima::fastdds::dds::TypeSupport type_;
    std::map<eprosima::fastrtps::rtps::InstanceHandle_t, eprosima::fastrtps::rtps::SequenceNumber_t> last_seq;
    size_t current_processed_count_;
    size_t number_samples_expected_;
    bool discovery_result_;

    std::string xml_file_ = "";
    std::string participant_profile_ = "";
    std::string datareader_profile_ = "";

    std::function<bool(const eprosima::fastrtps::rtps::ParticipantDiscoveryInfo& info)> onDiscovery_;
    std::function<bool(const eprosima::fastrtps::rtps::WriterDiscoveryInfo& info)> onEndpointDiscovery_;

    //! True to take data from history. False to read
    bool take_;

    //! True if the class is called from the statistics blackbox (specific topic name and domain id).
    bool statistics_;

#if HAVE_SECURITY
    std::mutex mutexAuthentication_;
    std::condition_variable cvAuthentication_;
    unsigned int authorized_;
    unsigned int unauthorized_;
#endif // if HAVE_SECURITY

    //! A mutex for liveliness status
    std::mutex liveliness_mutex_;
    //! A condition variable to notify when liveliness was recovered
    std::condition_variable liveliness_cv_;
    //! Number of times liveliness was lost
    unsigned int times_liveliness_lost_;
    //! Number of times liveliness was recovered
    unsigned int times_liveliness_recovered_;
    //! The liveliness changed status
    eprosima::fastrtps::LivelinessChangedStatus liveliness_changed_status_;

    //! A mutex for incompatible_qos status
    std::mutex incompatible_qos_mutex_;
    //! A condition variable to notify when incompatible qos was received
    std::condition_variable incompatible_qos_cv_;
    //! Number of times incompatible_qos was received
    unsigned int times_incompatible_qos_;
    //! Latest conflicting PolicyId
    eprosima::fastdds::dds::QosPolicyId_t last_incompatible_qos_;

    //! A mutex for messages received but not yet processed by the application
    std::mutex message_receive_mutex_;
    //! A condition variable for messages received but not yet processed by the application
    std::condition_variable message_receive_cv_;
    //! Number of messages received but not yet processed by the application
    std::atomic<size_t> message_receive_count_;
};

template<class TypeSupport>
class PubSubReaderWithWaitsets : public PubSubReader<TypeSupport>
{
public:

    typedef TypeSupport type_support;
    typedef typename type_support::type type;

protected:

    class WaitsetThread
    {
    public:

        WaitsetThread(
                PubSubReaderWithWaitsets& reader)
            : reader_(reader)
        {
        }

        ~WaitsetThread()
        {
            stop();
        }

        void start(
                const eprosima::fastrtps::Duration_t& timeout)
        {
            waitset_.attach_condition(reader_.datareader_->get_statuscondition());
            waitset_.attach_condition(reader_.subscriber_->get_statuscondition());
            waitset_.attach_condition(guard_condition_);

            std::unique_lock<std::mutex> lock(mutex_);
            if (nullptr == thread_)
            {
                running_ = true;
                guard_condition_.set_trigger_value(false);
                timeout_ = timeout;
                thread_ = new std::thread(&WaitsetThread::run, this);
            }
        }

        void stop()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            running_ = false;
            if (nullptr != thread_)
            {
                lock.unlock();

                // We need to trigger the wake up
                guard_condition_.set_trigger_value(true);
                thread_->join();
                lock.lock();
                delete thread_;
                thread_ = nullptr;
            }
        }

        void run()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while (running_)
            {
                lock.unlock();
                auto wait_result = waitset_.wait(active_conditions_, timeout_);
                if (wait_result == ReturnCode_t::RETCODE_TIMEOUT)
                {
                    reader_.on_waitset_timeout();
                }
                else
                {
                    if (!guard_condition_.get_trigger_value())
                    {
                        for (auto condition : active_conditions_)
                        {
                            process(dynamic_cast<eprosima::fastdds::dds::StatusCondition*>(condition));
                        }
                    }
                }
                lock.lock();
            }
        }

        void process(
                eprosima::fastdds::dds::StatusCondition* condition)
        {
            eprosima::fastdds::dds::StatusMask triggered_statuses = reader_.datareader_->get_status_changes();
            triggered_statuses &= condition->get_enabled_statuses();

            if (triggered_statuses.is_active(eprosima::fastdds::dds::StatusMask::subscription_matched()))
            {
                eprosima::fastdds::dds::SubscriptionMatchedStatus status;
                reader_.datareader_->get_subscription_matched_status(status);

                if (0 < status.current_count_change)
                {
                    std::cout << "Subscriber matched publisher " << status.last_publication_handle << std::endl;
                    reader_.matched();
                }
                else if (0 > status.current_count_change)
                {
                    std::cout << "Subscriber unmatched publisher " << status.last_publication_handle << std::endl;
                    reader_.unmatched();
                }
            }

            if (triggered_statuses.is_active(eprosima::fastdds::dds::StatusMask::requested_deadline_missed()))
            {
                eprosima::fastdds::dds::RequestedDeadlineMissedStatus status;
                reader_.datareader_->get_requested_deadline_missed_status(status);
                times_deadline_missed_ = status.total_count;
            }

            if (triggered_statuses.is_active(eprosima::fastdds::dds::StatusMask::requested_incompatible_qos()))
            {
                eprosima::fastdds::dds::RequestedIncompatibleQosStatus status;
                reader_.datareader_->get_requested_incompatible_qos_status(status);
                reader_.incompatible_qos(status);
            }

            if (triggered_statuses.is_active(eprosima::fastdds::dds::StatusMask::liveliness_changed()))
            {
                eprosima::fastdds::dds::LivelinessChangedStatus status;
                reader_.datareader_->get_liveliness_changed_status(status);

                reader_.set_liveliness_changed_status(status);
                if (status.alive_count_change == 1)
                {
                    reader_.liveliness_recovered();

                }
                else if (status.not_alive_count_change == 1)
                {
                    reader_.liveliness_lost();

                }
            }

            if (triggered_statuses.is_active(eprosima::fastdds::dds::StatusMask::data_available()))
            {
                {
                    std::lock_guard<std::mutex> guard(reader_.message_receive_mutex_);
                    reader_.message_receive_count_.fetch_add(1);
                }
                reader_.message_receive_cv_.notify_one();

                if (reader_.receiving_.load())
                {
                    bool ret = false;
                    do
                    {
                        reader_.receive_one(reader_.datareader_, ret);
                    } while (ret);
                }
            }

            // We also have to process the subscriber
            triggered_statuses = reader_.subscriber_->get_status_changes();
            triggered_statuses &= condition->get_enabled_statuses();

            if (triggered_statuses.is_active(eprosima::fastdds::dds::StatusMask::data_on_readers()))
            {
                {
                    std::lock_guard<std::mutex> guard(reader_.message_receive_mutex_);
                    reader_.message_receive_count_.fetch_add(1);
                }
                reader_.message_receive_cv_.notify_one();

                if (reader_.receiving_.load())
                {
                    bool ret = false;
                    do
                    {
                        reader_.receive_one(reader_.datareader_, ret);
                    } while (ret);
                }
            }
        }

        unsigned int missed_deadlines() const
        {
            return times_deadline_missed_;
        }

    protected:

        // The reader this waitset thread serves
        PubSubReaderWithWaitsets& reader_;

        // The waitset where the thread will be blocked
        eprosima::fastdds::dds::WaitSet waitset_;

        // The active conditions that triggered the wake up
        eprosima::fastdds::dds::ConditionSeq active_conditions_;

        // The thread that does the job
        std::thread* thread_ = nullptr;

        // Whether the thread is running or not
        bool running_ = false;

        // A Mutex to guard the thread start/stop
        std::mutex mutex_;

        // A user-triggered condition used to signal the thread to stop
        eprosima::fastdds::dds::GuardCondition guard_condition_;

        //! Number of times deadline was missed
        unsigned int times_deadline_missed_ = 0;

        //! The timeout for the wait operation
        eprosima::fastrtps::Duration_t timeout_;

    }
    waitset_thread_;

    friend class WaitsetThread;

public:

    PubSubReaderWithWaitsets(
            const std::string& topic_name,
            bool take = true,
            bool statistics = false)
        : PubSubReader<TypeSupport>(topic_name, take, statistics)
        , waitset_thread_(*this)
        , timeout_(eprosima::fastrtps::c_TimeInfinite)
        , times_waitset_timeout_(0)
    {
    }

    ~PubSubReaderWithWaitsets() override
    {
    }

    void createSubscriber() override
    {
        if (participant_ != nullptr)
        {
            // Create subscriber
            subscriber_ = participant_->create_subscriber(subscriber_qos_);
            ASSERT_NE(subscriber_, nullptr);
            ASSERT_TRUE(subscriber_->is_enabled());

            if (!xml_file_.empty())
            {
                if (!datareader_profile_.empty())
                {
                    datareader_ = subscriber_->create_datareader_with_profile(topic_, datareader_profile_, nullptr);
                    ASSERT_NE(datareader_, nullptr);
                    ASSERT_TRUE(datareader_->is_enabled());
                }
            }
            if (datareader_ == nullptr)
            {
                datareader_ = subscriber_->create_datareader(topic_, datareader_qos_, nullptr);
            }

            if (datareader_ != nullptr)
            {
                initialized_ = datareader_->is_enabled();
                if (initialized_)
                {
                    std::cout << "Created datareader " << datareader_->guid() << " for topic " <<
                        topic_name_ << std::endl;
                }

                // Set the desired status condition mask and start the waitset thread
                datareader_->get_statuscondition().set_enabled_statuses(status_mask_);
                subscriber_->get_statuscondition().set_enabled_statuses(status_mask_);
                waitset_thread_.start(timeout_);
            }
        }
    }

    void destroy() override
    {
        if (initialized_)
        {
            waitset_thread_.stop();
        }

        PubSubReader<TypeSupport>::destroy();
    }

    unsigned int missed_deadlines() const
    {
        return waitset_thread_.missed_deadlines();
    }

    void wait_waitset_timeout(
            unsigned int times = 1)
    {
        std::unique_lock<std::mutex> lock(waitset_timeout_mutex_);

        waitset_timeout_cv_.wait(lock, [&]()
                {
                    return times_waitset_timeout_ >= times;
                });
    }

    unsigned int times_waitset_timeout()
    {
        std::unique_lock<std::mutex> lock(waitset_timeout_mutex_);
        return times_waitset_timeout_;
    }

    PubSubReaderWithWaitsets& waitset_timeout(
            const eprosima::fastrtps::Duration_t& timeout)
    {
        timeout_ = timeout;
        return *this;
    }

protected:

    void on_waitset_timeout()
    {
        std::unique_lock<std::mutex> lock(waitset_timeout_mutex_);
        ++times_waitset_timeout_;
        waitset_timeout_cv_.notify_one();
    }

    //! The timeout for the waitset
    eprosima::fastrtps::Duration_t timeout_;

    //! A mutex for waitset timeout
    std::mutex waitset_timeout_mutex_;
    //! A condition variable to notify when the waitset has timed out
    std::condition_variable waitset_timeout_cv_;
    //! Number of times the waitset has timed out
    unsigned int times_waitset_timeout_;

    using PubSubReader<TypeSupport>::xml_file_;
    using PubSubReader<TypeSupport>::participant_;
    using PubSubReader<TypeSupport>::topic_name_;
    using PubSubReader<TypeSupport>::topic_;
    using PubSubReader<TypeSupport>::subscriber_;
    using PubSubReader<TypeSupport>::subscriber_qos_;
    using PubSubReader<TypeSupport>::datareader_;
    using PubSubReader<TypeSupport>::datareader_qos_;
    using PubSubReader<TypeSupport>::datareader_profile_;
    using PubSubReader<TypeSupport>::initialized_;
    using PubSubReader<TypeSupport>::status_mask_;
};

#endif // _TEST_BLACKBOX_PUBSUBREADER_HPP_
