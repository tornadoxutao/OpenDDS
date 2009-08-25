// -*- C++ -*-
//
// $Id$

#include "DCPS/DdsDcps_pch.h" //Only the _pch include should start with DCPS/
#include "ace/Event_Handler.h"
#include "ace/Reactor.h"
#include "InstanceState.h"
#include "DataReaderImpl.h"
#include "SubscriptionInstance.h"
#include "ReceivedDataElementList.h"
#include "Qos_Helper.h"

#if !defined (__ACE_INLINE__)
# include "InstanceState.inl"
#endif /* ! __ACE_INLINE__ */

OpenDDS::DCPS::InstanceState::InstanceState(DataReaderImpl* reader,
                                            ACE_Recursive_Thread_Mutex& lock,
                                            DDS::InstanceHandle_t handle)
  : lock_(lock),
    instance_state_(0),
    view_state_(0),
    disposed_generation_count_(0),
    no_writers_generation_count_(0),
    empty_(true),
    release_pending_(false),
    release_timer_id_(-1),
    reader_(reader),
    handle_(handle)
{}

OpenDDS::DCPS::InstanceState::~InstanceState()
{
  cancel_release();
}

// cannot ACE_INLINE because of #include loop

int
OpenDDS::DCPS::InstanceState::handle_timeout(const ACE_Time_Value& /* current_time */,
                                             const void* /* arg */)
{
  ACE_GUARD_RETURN(ACE_Recursive_Thread_Mutex,
                   guard,
                   this->lock_,
                   0);

  ACE_DEBUG((LM_WARNING,
             ACE_TEXT("(%P|%t) WARNING:")
             ACE_TEXT(" InstanceState::handle_timeout:")
             ACE_TEXT(" autopurging samples with instance handle 0x%x!\n"),
             this->handle_));

  this->release();

  return 0;
}


bool
OpenDDS::DCPS::InstanceState::dispose_was_received(const PublicationId& writer_id)
{
  writers_.erase (writer_id);

  //
  // Manage the instance state on disposal here.
  //
  if( this->instance_state_ & DDS::ALIVE_INSTANCE_STATE)
  {
    this->instance_state_ = DDS::NOT_ALIVE_DISPOSED_INSTANCE_STATE;
    schedule_release();
    return true;
  }
  return false;
}

bool
OpenDDS::DCPS::InstanceState::unregister_was_received(const PublicationId& writer_id)
{
  writers_.erase (writer_id);

  if(writers_.empty () && this->instance_state_ & DDS::ALIVE_INSTANCE_STATE)
  {
    this->instance_state_ = DDS::NOT_ALIVE_NO_WRITERS_INSTANCE_STATE;
    schedule_release();
    return true;
  }
  return false;
}

void
OpenDDS::DCPS::InstanceState::writer_became_dead (
  const PublicationId&  writer_id,
  int                   /*num_alive_writers*/,
  const ACE_Time_Value& /* when */)
{
  writers_.erase (writer_id);

  if(writers_.empty () && this->instance_state_ & DDS::ALIVE_INSTANCE_STATE)
  {
    this->instance_state_ = DDS::NOT_ALIVE_NO_WRITERS_INSTANCE_STATE;
    schedule_release();
  }
}

void
OpenDDS::DCPS::InstanceState::schedule_pending()
{
  this->release_pending_ = true;
}

void
OpenDDS::DCPS::InstanceState::schedule_release()
{
    DDS::DataReaderQos qos;
    this->reader_->get_qos(qos);

    DDS::Duration_t delay;
    switch (this->instance_state_)
    {
    case DDS::NOT_ALIVE_NO_WRITERS_INSTANCE_STATE:
      delay = qos.reader_data_lifecycle.autopurge_nowriter_samples_delay;
      break;

    case DDS::NOT_ALIVE_DISPOSED_INSTANCE_STATE:
      delay = qos.reader_data_lifecycle.autopurge_disposed_samples_delay;
      break;

    default:
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("(%P|%t) ERROR: InstanceState::schedule_release:")
                 ACE_TEXT(" Unsupported instance state: %d!\n"),
                 this->instance_state_));
      return;
    }

    if (delay.sec != DDS::DURATION_INFINITE_SEC &&
        delay.nanosec != DDS::DURATION_INFINITE_NSEC)
    {
      cancel_release();

      ACE_Reactor* reactor = this->reader_->get_reactor();
      reactor->schedule_timer(this, 0, duration_to_time_value(delay));

      if (this->release_timer_id_ == -1)
      {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("(%P|%t) ERROR: InstanceState::schedule_release:")
                   ACE_TEXT(" Unable to schedule timer!\n")));
      }
    }
    else
    {
      // N.B. instance transitions are always followed by a non-valid
      // sample being queued to the ReceivedDataElementList; marking
      // the release as pending prevents this sample from being lost
      // if all samples have been already removed from the instance.
      schedule_pending();
    }
}

void
OpenDDS::DCPS::InstanceState::cancel_release()
{
  this->release_pending_ = false;

  if (this->release_timer_id_ != -1)
  {
    ACE_Reactor* reactor = this->reader_->get_reactor();
    reactor->cancel_timer(this->release_timer_id_);

    this->release_timer_id_ = -1;
  }
}

void
OpenDDS::DCPS::InstanceState::release_if_empty()
{
  if( this->empty_ && this->writers_.empty())
  {
    release();
  }
  else
  {
    schedule_pending();
  }
}

void
OpenDDS::DCPS::InstanceState::release()
{
  this->reader_->release_instance(this->handle_);
}

