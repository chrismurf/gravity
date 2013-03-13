#include <iostream>
#include <queue>
#include <list>
#include <map>
#include <set>
#include <sstream>

#include <zmq.h>
#include "GravityNode.h"
#include "GravityHeartbeatListener.h"
#include "GravityHeartbeat.h"
#include "GravitySemaphore.h"
#include "GravityPublishManager.h"
#include "GravityLogger.h"
#include "CommUtil.h"

namespace gravity
{

using namespace std;

std::list<ExpectedMessageQueueElement> Heartbeat::queueElements; //This is so we can reuse these guys.
std::priority_queue<ExpectedMessageQueueElement*, vector<ExpectedMessageQueueElement*>, EMQComparator> Heartbeat::messageTimes;
std::map<std::string, GravityHeartbeatListener*> Heartbeat::listener;

Semaphore Heartbeat::lock;
std::set<std::string> Heartbeat::filledHeartbeats;


void Heartbeat::subscriptionFilled(const std::vector< shared_ptr<GravityDataProduct> >& dataProducts)
{
	lock.Lock();
	for(size_t i = 0; i < dataProducts.size(); i++)
		filledHeartbeats.insert(dataProducts[i]->getDataProductID());
	lock.Unlock();
}


void* Heartbeat::HeartbeatListenerThrFunc(void* thread_context)
{
	HBListenerContext* params = (HBListenerContext*) thread_context;

    void* hbSocket = zmq_socket(params->zmq_context, ZMQ_REP);
    zmq_connect(hbSocket, "inproc://heartbeat_listener");

    while(true)
    {
    	if(!messageTimes.empty())
    	{

			ExpectedMessageQueueElement& mqe = *messageTimes.top();

			if (mqe.expectedTime > getCurrentTime())
			{
		 		gravity::sleep(100);	
			}
			else
			{
				lock.Lock();
				std::set<std::string>::iterator i = filledHeartbeats.find(mqe.dataproductID);
				bool gotHeartbeat = (i != filledHeartbeats.end());
				if(gotHeartbeat)
					filledHeartbeats.erase(i);
				lock.Unlock();
	
				if(!gotHeartbeat)
				{
				    int diff = mqe.lastHeartbeatTime == 0 ? -1 : getCurrentTime() - mqe.lastHeartbeatTime;
					listener[mqe.dataproductID]->MissedHeartbeat(mqe.dataproductID, diff, mqe.timetowaitBetweenHeartbeats);
				}
				else
				{
					listener[mqe.dataproductID]->ReceivedHeartbeat(mqe.dataproductID, mqe.timetowaitBetweenHeartbeats);
					mqe.lastHeartbeatTime = getCurrentTime();
				}

				messageTimes.pop();
				mqe.expectedTime = getCurrentTime() + mqe.timetowaitBetweenHeartbeats; //(Maybe should be lastHeartbeatTime + timetowaitBetweenHeartbeats, but current version allows for drift etc.)
				messageTimes.push(&mqe);
			}
    	}//Process Messages

        //Allow Gravity to add Heartbeat listeners.
    	{
    		zmq_msg_t msg;
    	    zmq_msg_init(&msg);
    	    std::string dataproductID;
    	    //unsigned short port;
    	    int64_t maxtime;

    	    if(zmq_recvmsg(hbSocket, &msg, ZMQ_DONTWAIT) != -1)
    	    {
				//Receive Dataproduct ID
				int size = zmq_msg_size(&msg);
				char* s = (char*)malloc(size+1);
				memcpy(s, zmq_msg_data(&msg), size);
				s[size] = 0;
				dataproductID.assign(s, size);
				free(s);
				//zmq_msg_close(&msg); //Closed after the end of the if statement.

				//Receive address of listener
				zmq_msg_t msg2;
				zmq_msg_init(&msg2);
				intptr_t p;
				zmq_recvmsg(hbSocket, &msg2, ZMQ_DONTWAIT); //These are guarunteed to not fail since this is a multi part message.
				memcpy(&p, zmq_msg_data(&msg2), sizeof(intptr_t));
				zmq_msg_close(&msg2);

				listener[dataproductID] = (GravityHeartbeatListener*) p;

				//Receive maxtime.
				zmq_msg_t msg3;
				zmq_msg_init(&msg3);
				zmq_recvmsg(hbSocket, &msg3, ZMQ_DONTWAIT);
				memcpy(&maxtime, zmq_msg_data(&msg3), 8);
				zmq_msg_close(&msg3);

				ExpectedMessageQueueElement mqe1;
				mqe1.dataproductID = dataproductID;
				mqe1.expectedTime = getCurrentTime() + maxtime;
				mqe1.lastHeartbeatTime = 0;
				mqe1.timetowaitBetweenHeartbeats = maxtime;

				// Send ACK
				sendStringMessage(hbSocket, "ACK", ZMQ_DONTWAIT);

				//We should already be subscribed to the Heartbeats.

				queueElements.push_back(mqe1);
				messageTimes.push(&queueElements.back());
    	    }

	    	zmq_msg_close(&msg);

    	}//Add Heartbeat listeners

    }//while(true)

	//This will never be reached but should be done when the thread ends.
    delete params;

    return NULL;
}

void *heartbeatSocket;
void bindHeartbeatSocket(void* context)
{
    heartbeatSocket = zmq_socket(context, ZMQ_PUB);
    zmq_bind(heartbeatSocket, PUB_MGR_HB_URL);
}

void closeHeartbeatSocket()
{
    zmq_close(heartbeatSocket);
}

void* Heartbeat(void* thread_context)
{
	HBParams* params = (HBParams*) thread_context;	
	GravityDataProduct gdp(params->componentID);
	gdp.setData((void*)"Good", 5);

	while(true)
	{
		// Publish heartbeat (via the GravityPublishManager)
		gdp.setTimestamp(getCurrentTime());
		Log::trace("%s: Publishing heartbeat", params->componentID.c_str());
		sendStringMessage(heartbeatSocket, "publish", ZMQ_SNDMORE);
		sendStringMessage(heartbeatSocket, "", ZMQ_SNDMORE);
		zmq_msg_t msg;
		zmq_msg_init_size(&msg, gdp.getSize());
		gdp.serializeToArray(zmq_msg_data(&msg));
		zmq_sendmsg(heartbeatSocket, &msg, ZMQ_DONTWAIT);
		zmq_msg_close(&msg);
#ifdef WIN32
		Sleep(params->interval_in_microseconds/1000);
#else
		struct timespec request;
		request.tv_sec = params->interval_in_microseconds / 1000000;
		request.tv_nsec = (params->interval_in_microseconds % 1000000)*1000; //Convert from Microseconds to Nanoseconds
		clock_nanosleep(CLOCK_REALTIME, 0, &request, NULL);
#endif
	}

	//This will never be reached but should be done when the thread ends.
	delete params;

	return NULL;
}

} //namespace gravity
