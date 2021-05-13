#include "EigenharpCore.h"

EigenharpCore::EigenharpCore() : eigenApi("./"), osc(&oscSendQueue, &oscReceiveQueue) {
    jassert(coreInstance == nullptr);
    coreInstance = this;
}

void EigenharpCore::initialise(const juce::String &) {
    exitThreads = false;
    signal(SIGINT, EigenharpCore::intHandler);
    osc.connectSender("127.0.0.1", senderPort);
    osc.connectReceiver(receiverPort);

    eigenApi.setPollTime(EIGENAPI_POLLTIME);
    apiCallback = new APICallback(eigenApi, &oscSendQueue);
    eigenApi.addCallback(apiCallback);
    if(!eigenApi.start()) {
        std::cout  << "unable to start EigenLite" << std::endl;
    }
    
    eigenApiProcessThread = std::thread(coreInstance->eigenharpProcess, &oscReceiveQueue, &eigenApi);
}

void EigenharpCore::shutdown() {
    std::cout << "Trying to quit gracefully..." << std::endl;
    exitThreads = true;
    sleep(1);
    eigenApiProcessThread.join();
    eigenApi.stop();
    osc.disconnectReceiver();
    osc.disconnectSender();
    
    delete apiCallback;
}

void EigenharpCore::intHandler(int dummy) {
    std::cerr  << "int handler called" << std::endl;
    coreInstance->systemRequestedQuit();
}

void* EigenharpCore::eigenharpProcess(OSC::OSCMessageFifo *msgQueue, void* arg) {
    EigenApi::Eigenharp *pE = static_cast<EigenApi::Eigenharp*>(arg);
    while(!exitThreads) {

#ifdef MEASURE_EIGENAPIPROCESSTIME
        static int counter = 0;
        auto begin = std::chrono::high_resolution_clock::now();
#endif

        pE->process();
        static OSC::Message msg;
        if (msgQueue->getMessageCount() > 0) {
            msgQueue->read(&msg);
            if (msg.type == OSC::MessageType::LED)
                pE->setLED(deviceId, msg.course, msg.key, msg.colour);
        }
        
#ifdef MEASURE_EIGENAPIPROCESSTIME
        auto end = std::chrono::high_resolution_clock::now();
        if (counter%1000 == 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin);
            std::cout << "EigenharpProcess time: " << elapsed.count() << std::endl;
        }
#endif
        
        std::this_thread::sleep_for(std::chrono::microseconds(PROCESS_MICROSEC_SLEEP));
    }
    return nullptr;
}

//void EigenharpCore::makeThreadRealtime(std::thread& thread) {
//
//    int policy;
//    struct sched_param param;
//
//    pthread_getschedparam(thread.native_handle(), &policy, &param);
//    param.sched_priority = 95;
//    pthread_setschedparam(thread.native_handle(), SCHED_FIFO, &param);
//
//}
