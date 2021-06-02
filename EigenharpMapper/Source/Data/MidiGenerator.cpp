#include "MidiGenerator.h"

MidiGenerator::MidiGenerator(ConfigLookup (&configLookups)[3]) {
    this->configLookups = configLookups;
}

MidiGenerator::~MidiGenerator() {
    delete lowerChanAssigner;
    delete upperChanAssigner;
}

void MidiGenerator::start() {
    int lowerChannelCount = SettingsWrapper::getLowerMPEVoiceCount();
    mpeZone.setLowerZone(lowerChannelCount, 2, SettingsWrapper::getLowerMPEPB());
    lowerChanAssigner = new juce::MPEChannelAssigner(mpeZone.getLowerZone());
    if (lowerChannelCount < 14) {
        int upperChannelCount = SettingsWrapper::getUpperMPEVoiceCount();
        mpeZone.setUpperZone(upperChannelCount, 2, SettingsWrapper::getUpperMPEPB());
        upperChanAssigner = new juce::MPEChannelAssigner(mpeZone.getUpperZone());
    }
    else
        upperChanAssigner = nullptr;
    
    configLookups[0].updateAll();
    configLookups[1].updateAll();
    configLookups[2].updateAll();

    samplesSinceLastBreathMsg = 0;
    breathMessageCount = 0;
    stripMessageCount[0] = 0;
    stripMessageCount[1] = 0;
    initialized = true;
}

void MidiGenerator::stop() {
    initialized = false;
    delete lowerChanAssigner;
    delete upperChanAssigner;
    lowerChanAssigner = nullptr;
    upperChanAssigner = nullptr;
}

void MidiGenerator::processOSCMessage(OSC::Message &oscMsg, juce::MidiBuffer &midiBuffer) {
    if (!initialized)
        return;
    
    switch (oscMsg.type) {
        case OSC::MessageType::Key: {
                int deviceIndex = (int)oscMsg.device -1;
                KeyState *keyState = &keyStates[deviceIndex][oscMsg.course][oscMsg.key];
                keyState->messageCount++;
                keyState->ehPressure = oscMsg.pressure;
                keyState->ehYaw = oscMsg.yaw;
                keyState->ehRoll = oscMsg.roll;

                ConfigLookup::Key keyLookup = configLookups[deviceIndex].keys[oscMsg.course][oscMsg.key];
                if (keyLookup.output == MidiChannelType::Undefined)
                    break;
                
                if (keyState->status == KeyStatus::Off && oscMsg.active) {
                    keyState->status = KeyStatus::Pending;
                }
                else if (keyState->messageCount == 4 && keyState->status == KeyStatus::Pending && oscMsg.active) {
                    createNoteOn(keyLookup, keyState, midiBuffer);
                }
                else if (keyState->status != KeyStatus::Off && !oscMsg.active) {
                    createNoteOff(keyLookup, keyState, midiBuffer);
                }
                else if (keyState->messageCount == 16 && keyState->status != KeyStatus::Pending) {
                    createNoteHold(keyLookup, keyState, midiBuffer);
                }
            }
            break;
        case OSC::MessageType::Breath: {
                breathMessageCount++;
                int deviceIndex = (int)oscMsg.device -1;
                ehBreath[deviceIndex] = std::abs((int)(oscMsg.value - 2048))*2;
                if (breathMessageCount == 16) {
                    createBreath(deviceIndex, configLookups[deviceIndex], midiBuffer);
                }
            }
            break;
        case OSC::MessageType::Strip: {
                stripMessageCount[oscMsg.strip]++;
                int deviceIndex = (int)oscMsg.device -1;
                ehStrip1[deviceIndex] = std::abs((int)(oscMsg.value - 2048))*2;
                if (stripMessageCount[oscMsg.strip] == 16) {
                    createStripAbsolute(deviceIndex, oscMsg.strip, configLookups[deviceIndex], midiBuffer);
                    createStripRelative(deviceIndex, oscMsg.strip, configLookups[deviceIndex], midiBuffer);
                }
            }
            break;
        default:
            break;
    }
}

void MidiGenerator::reduceBreath(juce::MidiBuffer &buffer) {
    for (int i = 0; i < 3; i++) {
        if (ehBreath[i] <= 0) {
            ehBreath[i] = 0;
            continue;
        }
        
        ehBreath[i] -= 20;
        ehBreath[i] = std::max<int>(ehBreath[i], 0);
        createBreath(i, configLookups[i], buffer);
    }
}

void MidiGenerator::createBreath(int deviceIndex, ConfigLookup &keyLookup, juce::MidiBuffer &buffer) {

    addMidiValueMessage(keyLookup.breath[0].channel, ehBreath[deviceIndex], keyLookup.breath[0].midiValue, keyLookup.keys[0][0], buffer, false);
    addMidiValueMessage(keyLookup.breath[1].channel, ehBreath[deviceIndex], keyLookup.breath[1].midiValue, keyLookup.keys[0][0], buffer, false);
    addMidiValueMessage(keyLookup.breath[2].channel, ehBreath[deviceIndex], keyLookup.breath[2].midiValue, keyLookup.keys[0][0], buffer, false);

    breathMessageCount = 0;
    samplesSinceLastBreathMsg = 0;
}

void MidiGenerator::createStripAbsolute(int deviceIndex, int stripNo, ConfigLookup &keyLookup, juce::MidiBuffer &buffer) {
    
}

void MidiGenerator::createStripRelative(int deviceIndex, int stripNo, ConfigLookup &keyLookup, juce::MidiBuffer &buffer) {
    
}

void MidiGenerator::createNoteOn(ConfigLookup::Key &keyLookup, KeyState *state, juce::MidiBuffer &buffer) {
    if (keyLookup.output == MidiChannelType::MPE_Low)
        state->midiChannel = lowerChanAssigner->findMidiChannelForNewNote(keyLookup.note);
    else if (keyLookup.output == MidiChannelType::MPE_High) {
        if (upperChanAssigner != nullptr)
            state->midiChannel = upperChanAssigner->findMidiChannelForNewNote(keyLookup.note);
    }
    else
        state->midiChannel = (int)keyLookup.output;
    
    createNoteHold(keyLookup, state, buffer);
    auto vel = juce::MPEValue::from7BitInt(unipolar(state->ehPressure*4)*126+1);
    auto noteOnMsg = juce::MidiMessage::noteOn(state->midiChannel, keyLookup.note, vel.asUnsignedFloat());
    buffer.addEvent(noteOnMsg, buffer.getLastEventTime()+1);
    
    state->status = KeyStatus::Active;
}

void MidiGenerator::createNoteOff(ConfigLookup::Key &keyLookup, KeyState *state, juce::MidiBuffer &buffer) {
    int channel = state->midiChannel;
    if (keyLookup.output == MidiChannelType::MPE_Low)
        lowerChanAssigner->noteOff(keyLookup.note, channel);
    else if (keyLookup.output == MidiChannelType::MPE_High) {
        if (upperChanAssigner != nullptr)
            upperChanAssigner->noteOff(keyLookup.note, channel);
    }

    auto noteOffMsg = juce::MidiMessage::noteOff(channel, keyLookup.note, 0.0f);
    buffer.addEvent(noteOffMsg, 0);
    addMidiValueMessage(channel, 0, keyLookup.pressure, keyLookup, buffer, false);
    state->status = KeyStatus::Off;
    state->messageCount = 0;
}

void MidiGenerator::createNoteHold(ConfigLookup::Key &keyLookup, KeyState *state, juce::MidiBuffer &buffer) {
    int channel = state->midiChannel;
    addMidiValueMessage(channel, state->ehRoll, keyLookup.roll, keyLookup, buffer, true);
    addMidiValueMessage(channel, state->ehYaw, keyLookup.yaw, keyLookup, buffer, true);
    addMidiValueMessage(channel, state->ehPressure, keyLookup.pressure, keyLookup, buffer, false);
    state->messageCount = 0;
}

void MidiGenerator::addMidiValueMessage(int channel, int ehValue, ZoneWrapper::MidiValue midiValue, ConfigLookup::Key &keyLookup, juce::MidiBuffer &buffer, bool isBipolar) {
    if (midiValue.valueType != MidiValueType::Off) {
        juce::MidiMessage msg;
        if (midiValue.valueType == MidiValueType::Pitchbend) {
            auto pb = isBipolar ? juce::MPEValue::from14BitInt((calculatePitchBendCurve(bipolar(ehValue*1.7f))*keyLookup.pbRange)*0x1fff + 0x1fff) : juce::MPEValue::from14BitInt((unipolar(ehValue)*keyLookup.pbRange)*0x1fff + 0x1fff);
            msg = juce::MidiMessage::pitchWheel(channel, pb.as14BitInt());
        }
        else if (midiValue.valueType == MidiValueType::ChannelAftertouch) {
            auto at = isBipolar ? juce::MPEValue::from7BitInt(bipolar(ehValue*1.7f)*63+64)
                                : juce::MPEValue::from7BitInt(unipolar(ehValue*1.7f)*127);
            msg = juce::MidiMessage::channelPressureChange(channel, at.as7BitInt());
        }
        else if (midiValue.valueType == MidiValueType::PolyAftertouch) {
            auto at = isBipolar ? juce::MPEValue::from7BitInt(bipolar(ehValue*1.7f)*63+64)
                                : juce::MPEValue::from7BitInt(unipolar(ehValue*1.7f)*127);
            msg = juce::MidiMessage::aftertouchChange(channel, keyLookup.note, at.as7BitInt());
        }
        else if (midiValue.valueType == MidiValueType::CC) {
            auto cc = isBipolar ? juce::MPEValue::from7BitInt(bipolar(ehValue*1.7f)*63+64)
                                : juce::MPEValue::from7BitInt(unipolar(ehValue)*127);
            msg = juce::MidiMessage::controllerEvent(channel, midiValue.ccNo, cc.as7BitInt());
        }
        buffer.addEvent(msg, buffer.getLastEventTime()+1);
    }
}

void MidiGenerator::createLayoutRPNs(juce::MidiBuffer &buffer) {
    buffer.clear();
    auto buff = juce::MPEMessages::setZoneLayout(mpeZone);
    buffer.addEvents(buffer, 0, -1, 0);
}

float MidiGenerator::calculatePitchBendCurve(float value) {
    return clamp((tan(value)/3.14159265f) * 2.0f, -1.0, 1.0);
}
