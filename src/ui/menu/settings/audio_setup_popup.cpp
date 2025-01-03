#include "audio_setup_popup.hpp"

#ifdef GLOBED_VOICE_SUPPORT

#include "audio_device_cell.hpp"
#include <audio/manager.hpp>
#include <audio/voice_playback_manager.hpp>
#include <managers/settings.hpp>
#include <util/misc.hpp>
#include <util/ui.hpp>

using namespace geode::prelude;

bool AudioSetupPopup::setup() {
    this->setID("AudioSetupPopup"_spr);

    auto rlayout = util::ui::getPopupLayoutAnchored(m_size);

    auto menu = Build<CCMenu>::create()
        .pos(0.f, 0.f)
        .parent(m_mainLayer);

    Build<CCMenu>::create()
        .pos(rlayout.fromCenter(0.f, -110.f))
        .layout(RowLayout::create()
                    ->setGap(5.0f)
                    ->setAxisReverse(true)
        )
        .parent(m_mainLayer)
        .id("audio-visualizer-menu"_spr)
        .store(visualizerLayout);

    // record button
    recordButton = Build<CCSprite>::createSpriteName("GJ_playBtn2_001.png")
        .scale(0.485f)
        .intoMenuItem([this](auto) {
            auto& vpm = VoicePlaybackManager::get();
            vpm.prepareStream(-1);

            auto& vm = GlobedAudioManager::get();
            vm.setRecordBufferCapacity(1);
            auto result = vm.startRecordingRaw([this, &vpm](const float* pcm, size_t samples) {
                // calculate the avg audio volume
                this->audioLevel = util::misc::calculatePcmVolume(pcm, samples);

                // play back the audio
                vpm.playRawDataStreamed(-1, pcm, samples);
            });

            if (result.isErr()) {
                log::warn("failed to start recording: {}", result.unwrapErr());
                Notification::create(result.unwrapErr(), NotificationIcon::Error)->show();
                return;
            }

            this->toggleButtons(true);
            this->audioVisualizer->resetMaxVolume();
        })
        .parent(visualizerLayout)
        .id("record-button"_spr)
        .collect();

    // stop recording button
    stopRecordButton = Build<CCSprite>::createSpriteName("GJ_stopEditorBtn_001.png")
        .intoMenuItem([this](auto) {
            this->toggleButtons(false);

            auto& vm = GlobedAudioManager::get();
            vm.haltRecording();

            auto& vpm = VoicePlaybackManager::get();
            vpm.removeStream(-1);
        })
        .parent(visualizerLayout)
        .id("stop-recording-button"_spr)
        .collect();

    // refresh list button
    Build<CCSprite>::createSpriteName("GJ_updateBtn_001.png")
        .intoMenuItem([this](auto) {
            this->refreshList();
        })
        .pos(rlayout.fromBottomRight(5.f, 5.f))
        .parent(menu)
        .id("refresh-btn"_spr);

    Build<GlobedAudioVisualizer>::create()
        .parent(visualizerLayout)
        .id("audio-visualizer"_spr)
        .store(audioVisualizer);

    this->toggleButtons(false);

    Build(DeviceList::createForComments(LIST_WIDTH, LIST_HEIGHT, AudioDeviceCell::CELL_HEIGHT))
        .anchorPoint(0.5f, 1.f)
        .pos(rlayout.centerTop - CCPoint{0.f, 20.f})
        .parent(m_mainLayer)
        .store(listLayer);

    this->refreshList();

    this->scheduleUpdate();

    return true;
}

void AudioSetupPopup::update(float) {
    audioVisualizer->setVolume(audioLevel);
}

cocos2d::CCArray* AudioSetupPopup::createDeviceCells() {
    auto cells = CCArray::create();

    auto& vm = GlobedAudioManager::get();

    int activeId = -1;
    if (const auto& dev = vm.getRecordingDevice()) {
        activeId = dev->id;
    }

    auto devices = vm.getRecordingDevices();

    for (const auto& device : devices) {
        cells->addObject(AudioDeviceCell::create(device, this, activeId));
    }

    return cells;
}

void AudioSetupPopup::refreshList() {
    listLayer->swapCells(createDeviceCells());

    geode::cocos::handleTouchPriority(this);
}

void AudioSetupPopup::weakRefreshList() {
    auto& vm = GlobedAudioManager::get();
    auto recordDevices = vm.getRecordingDevices();
    size_t existingCount = listLayer->cellCount();
    if (existingCount != recordDevices.size()) {
        // if different device count, hard refresh
        this->refreshList();
        return;
    }

    int activeId = -1;
    if (const auto& dev = vm.getRecordingDevice()) {
        activeId = dev->id;
    }

    size_t refreshed = 0;
    for (auto* cell : *listLayer) {
        for (auto& rdev : recordDevices) {
            if (rdev.id == cell->deviceInfo.id) {
                cell->refreshDevice(rdev, activeId);
                refreshed++;
            }
        }
    }

    // if the wrong amount of cells was refreshed, hard refresh
    if (refreshed != existingCount) {
        this->refreshList();
    }
}

void AudioSetupPopup::onClose(cocos2d::CCObject* sender) {
    Popup::onClose(sender);
    auto& vm = GlobedAudioManager::get();
    vm.haltRecording();
}

void AudioSetupPopup::toggleButtons(bool recording) {
    recordButton->removeFromParent();
    stopRecordButton->removeFromParent();

    if (recording) {
        visualizerLayout->addChild(stopRecordButton);
    } else {
        visualizerLayout->addChild(recordButton);
    }

    visualizerLayout->updateLayout();
}

void AudioSetupPopup::applyAudioDevice(int id) {
    auto& vm = GlobedAudioManager::get();
    if (vm.isRecording()) {
        Notification::create("Cannot switch device while recording", NotificationIcon::Error, 3.0f)->show();
        return;
    }

    GlobedAudioManager::get().setActiveRecordingDevice(id);
    auto& settings = GlobedSettings::get();
    settings.communication.audioDevice = id;

    this->weakRefreshList();
}

AudioSetupPopup* AudioSetupPopup::create() {
    auto ret = new AudioSetupPopup;
    if (ret->initAnchored(POPUP_WIDTH, POPUP_HEIGHT)) {
        ret->autorelease();
        return ret;
    }

    delete ret;
    return nullptr;
}

#endif // GLOBED_VOICE_SUPPORT
