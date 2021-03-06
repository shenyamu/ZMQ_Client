/**
* Copyright (C) 2013 kangliqiang ,kangliq@163.com
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <stdio.h>
#include "ProcessQueue.h"
#include "MessageExt.h"
#include "KPRUtil.h"
#include "ScopedLock.h"

// 客户端本地Lock存活最大时间，超过则自动过期，单位ms
//"rocketmq.client.rebalance.lockMaxLiveTime", "30000"
unsigned int ProcessQueue::s_RebalanceLockMaxLiveTime = 30000;

// 定时Lock间隔时间，单位ms
//"rocketmq.client.rebalance.lockInterval", "20000"
unsigned int ProcessQueue::s_RebalanceLockInterval = 20000;

ProcessQueue::ProcessQueue()
{
	m_queueOffsetMax = 0L;
	m_msgCount=0;
	m_droped = false;

	m_locked = false;
	m_lastLockTimestamp = GetCurrentTimeMillis();
	m_consuming = false;

}

bool ProcessQueue::isLockExpired()
{
	bool result = (GetCurrentTimeMillis() - m_lastLockTimestamp) > s_RebalanceLockMaxLiveTime;
	return result;
}

bool ProcessQueue::putMessage(const std::list<MessageExt*>& msgs)
{
	bool dispathToConsume = false;
	try
	{
		kpr::ScopedLock<kpr::Mutex> lock(m_lockTreeMap);
		std::list<MessageExt*>::const_iterator it = msgs.begin();
		for (;it!=msgs.end();it++)
		{
			MessageExt* msg = (*it);
			m_msgTreeMap[msg->getQueueOffset()]= msg;
			m_queueOffsetMax = msg->getQueueOffset();
		}

		m_msgCount+=msgs.size();

		if (!m_msgTreeMap.empty() && !m_consuming)
		{
			dispathToConsume = true;
			m_consuming = true;
		}
	}
	catch (...)
	{
	}

	return dispathToConsume;
}

/**
* 获取当前队列的最大跨度
*/
long long ProcessQueue::getMaxSpan()
{
	try
	{
		kpr::ScopedLock<kpr::Mutex> lock(m_lockTreeMap);

		if (!m_msgTreeMap.empty())
		{
			std::map<long long, MessageExt*>::iterator it1 = m_msgTreeMap.begin();
			std::map<long long, MessageExt*>::iterator it2 = m_msgTreeMap.end();
			it2--;
			return it2->first - it1->first;
		}
	}
	catch (...)
	{
	}

	return 0;
}

long long ProcessQueue::removeMessage(const std::list<MessageExt*>& msgs)
{
	long long result = -1;
    long long origin = -1;
	try
	{
		kpr::ScopedLock<kpr::Mutex> lock(m_lockTreeMap);

		if (!m_msgTreeMap.empty())
		{
            /* modified by yu.guangjie at 2015-08-29, reason: use origin */
		    origin = m_msgTreeMap.begin()->first;
            
			result = m_queueOffsetMax+1;
			std::list<MessageExt*>::const_iterator it = msgs.begin();
			for (;it!=msgs.end();it++)
			{
				MessageExt* msg = (*it);
				m_msgTreeMap.erase(msg->getQueueOffset());
			}

			m_msgCount-=msgs.size();
		}

		if (!m_msgTreeMap.empty())
		{
			std::map<long long, MessageExt*>::iterator it = m_msgTreeMap.begin();
            if(origin != it->first)
            {
                result = it->first;
            }
            else
            {   // needn't to update offset
                result = -1;
            }			
		}
	}
	catch (...)
	{
	    printf("ProcessQueue::removeMessage() error!\n");
	}

	return result;
}

std::map<long long, MessageExt*> ProcessQueue::getMsgTreeMap()
{
	return m_msgTreeMap;
}

AtomicLong ProcessQueue::getMsgCount()
{
	return m_msgCount;
}

bool ProcessQueue::isDroped()
{
	return m_droped;
}

void ProcessQueue::setDroped(bool droped)
{
	m_droped = droped;
}

/**
* ========================================================================
* 以下部分为顺序消息专有操作
*/

void ProcessQueue::setLocked(bool locked)
{
	m_locked = locked;
}

bool ProcessQueue::isLocked()
{
	return m_locked;
}

void ProcessQueue::rollback()
{
	try
	{
		kpr::ScopedLock<kpr::Mutex> lock(m_lockTreeMap);
        /* modified by yu.guangjie at 2015-08-27, reason: rollback m_msgTreeMapTemp */
        std::map<long long, MessageExt*>::iterator it = m_msgTreeMapTemp.begin();
		for (; it != m_msgTreeMapTemp.end(); it++)
		{
			m_msgTreeMap[it->first] = it->second;
		}
        m_msgTreeMapTemp.clear();
	}
	catch (...)
	{
	    printf("ProcessQueue::rollback() error!\n");
	}
}

long long ProcessQueue::commit()
{
	try
	{
		kpr::ScopedLock<kpr::Mutex> lock(m_lockTreeMap);
		if (!m_msgTreeMapTemp.empty())
		{
			std::map<long long, MessageExt*>::iterator it = m_msgTreeMapTemp.end();
			it--;
			long long offset = it->first;
			m_msgCount-= m_msgTreeMapTemp.size();
			m_msgTreeMapTemp.clear();
			return offset + 1;
		}
	}
	catch (...)
	{
	    printf("ProcessQueue::commit() error!\n");
	}

	return -1;
}

void ProcessQueue::makeMessageToCosumeAgain(const std::list<MessageExt*>& msgs)
{
	try
	{
		// 临时Table删除
		// 正常Table增加
		kpr::ScopedLock<kpr::Mutex> lock(m_lockTreeMap);
		std::list<MessageExt*>::const_iterator it = msgs.begin();
		for (;it!=msgs.end();it++)
		{
			MessageExt* msg = (*it);
			m_msgTreeMapTemp.erase(msg->getQueueOffset());
			m_msgTreeMap[msg->getQueueOffset()]=msg;
		}
	}
	catch (...)
	{
	    printf("ProcessQueue::makeMessageToCosumeAgain() error!\n");
	}
}

/**
* 如果取不到消息，则将正在消费状态置为false
*
* @param batchSize
* @return
*/
std::list<MessageExt*> ProcessQueue::takeMessags(int batchSize)
{
	std::list<MessageExt*> result;
	try
	{
		kpr::ScopedLock<kpr::Mutex> lock(m_lockTreeMap);

		if (!m_msgTreeMap.empty())
		{
			for (int i = 0; i < batchSize; i++)
			{
				std::map<long long, MessageExt*>::iterator it=m_msgTreeMap.begin();
				if (it!=m_msgTreeMap.end())
				{
					result.push_back(it->second);
					m_msgTreeMapTemp[it->first]=it->second;
					m_msgTreeMap.erase(it);
				}
				else
				{
					break;
				}
			}
		}
	}
	catch (...)
	{
	    printf("ProcessQueue::takeMessags() error!\n");
	}

	if (result.empty())
	{
		m_consuming=false;
	}

	return result;
}

long long ProcessQueue::getLastLockTimestamp()
{
	return m_lastLockTimestamp;
}

void ProcessQueue::setLastLockTimestamp(long long lastLockTimestamp)
{
	m_lastLockTimestamp = lastLockTimestamp;
}
