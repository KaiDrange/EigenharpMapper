#include "EigenharpCore.h"

EigenharpCore::EigenharpCore() : eigenApi("./"), osc(&oscSendQueue, &oscReceiveQueue) {
    jassert(coreInstance == nullptr);
    coreInstance = this;
    mapperConnected = false;
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
    turnOffAllLEDs(&eigenApi);
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

void EigenharpCore::turnOffAllLEDs(EigenApi::Eigenharp *api) {
    int course0Length = 0;
    int course1Length = 0;
    int course2Length = 0;
    for (auto device : connectedDevices) {
        switch (device.type) {
            case EHDeviceType::Pico:
                course0Length = 18;
                course1Length = 4;
                break;
            case EHDeviceType::Tau:
                course0Length = 72;
                course1Length = 12;
                course2Length = 4; //TODO: Check if Tau has 2x4 or 8
                break;
            case EHDeviceType::Alpha:
                course0Length = 120;
                course1Length = 12;
                course2Length = 0;
                break;
            default:
                break;
        }

        for (int i = 0; i < course0Length; i++) {
            api->setLED(device.dev, 0, i, 0);
            device.assignedLEDColours[0][i] = 0;
            device.activeKeys[0][i] = false;
        }
        for (int i = 0; i < course1Length; i++) {
            api->setLED(device.dev, 1, i, 0);
            device.assignedLEDColours[1][i] = 0;
            device.activeKeys[1][i] = false;
        }
        for (int i = 0; i < course2Length; i++) {
            api->setLED(device.dev, 2, i, 0);
            device.assignedLEDColours[2][i] = 0;
            device.activeKeys[2][i] = false;
        }
    }
}

void* EigenharpCore::eigenharpProcess(OSC::OSCMessageFifo *msgQueue, void* arg) {
    EigenApi::Eigenharp *pE = static_cast<EigenApi::Eigenharp*>(arg);
    while(!exitThreads) {

#ifdef MEASURE_EIGENAPIPROCESSTIME
        static int counter = 0;
        auto begin = std::chrono::high_resolution_clock::now();
#endif
        static bool prevMapperConnectedState = false;
        if (mapperConnected != prevMapperConnectedState) {
            if (mapperConnected == false)
                turnOffAllLEDs(pE);
            prevMapperConnectedState = mapperConnected;
        }
        
        if (mapperConnected) {
            pE->process();
            static OSC::Message msg;
            if (msgQueue->getMessageCount() > 0) {
                msgQueue->read(&msg);
                if (msg.type == OSC::MessageType::LED) {
                    for (auto i = begin(connectedDevices); i != end(connectedDevices); i++) {
                        if (msg.device == i->type) {
                            i->assignedLEDColours[msg.course][msg.key] = msg.value;
                            pE->setLED(i->dev, msg.course, msg.key, msg.value);
                            break;
                        }
                    }
                }
            }
        }
        
#ifdef MEASURE_EIGENAPIPROCESSTIME
        auto end = std::chrono::high_resolution_clock::now();
        if (counter%1000 == 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin);
            std::cout << "EigenharpProcess time: " << elapsed.count() << std::endl;
        }
#endif
        
        std::this_thread::sleep_for(std::chrono::microseconds(PROCESS_MICROSEC_SLEEP + 100000*!mapperConnected));
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
