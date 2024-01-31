#include "interpolator.hpp"

# include "lerp_logger.hpp"
#include <hooks/play_layer.hpp>
#include <util/math.hpp>
#include <util/debugging.hpp>
#include <util/formatting.hpp>

using namespace geode::prelude;

PlayerInterpolator::PlayerInterpolator(const InterpolatorSettings& settings) : settings(settings) {}

void PlayerInterpolator::addPlayer(uint32_t playerId) {
    players.emplace(playerId, PlayerState {});
#ifdef GLOBED_DEBUG_INTERPOLATION
    LerpLogger::get().reset(playerId);
#endif
}

void PlayerInterpolator::removePlayer(uint32_t playerId) {
    players.erase(playerId);
}

void PlayerInterpolator::updatePlayer(uint32_t playerId, const PlayerData& data, float updateCounter) {
    auto& player = players.at(playerId);
    player.updateCounter = updateCounter;
    player.pendingRealFrame = true;
    player.totalFrames++;

    if (!util::math::equal(player.lastDeathTimestamp, data.lastDeathTimestamp)) {
        player.lastDeathTimestamp = data.lastDeathTimestamp;
        if (player.totalFrames > 1) {
            player.pendingDeath = true;
        }
    }

    LerpLogger::get().logRealFrame(playerId, this->getLocalTs(), data.timestamp, data.player1);

    if (settings.realtime) {
        player.interpolatedState.player1 = data.player1;
        player.interpolatedState.player2 = data.player2;
        return;
    }

    player.olderFrame = player.newerFrame;
    player.newerFrame = data;

    // TODO potential place for improvement - doing this every packet may or may not be stupid.
    // but for now it seems to work fine.
    player.timeCounter = player.olderFrame.timestamp;
}

static inline void lerpSpecific(
    const SpecificIconData& older,
    const SpecificIconData& newer,
    SpecificIconData& out,
    float lerpRatio
    ) {

    // misc variables
    out.iconType = older.iconType;
    out.isDashing = older.isDashing;
    out.isLookingLeft = older.isLookingLeft;
    out.isUpsideDown = older.isUpsideDown;
    out.isVisible = older.isVisible;
    out.isMini = older.isMini;

    // i hate spider
    if (out.iconType == PlayerIconType::Spider && std::abs(older.position.y - newer.position.y) >= 33.f) {
        out.position.x = std::lerp(older.position.x, newer.position.x, lerpRatio);
        out.position.y = older.position.y;
    } else {
        out.position = older.position.lerp(newer.position, lerpRatio);
    }
    out.rotation = std::lerp(older.rotation, newer.rotation, lerpRatio);
}

static inline void lerpPlayer(
    const VisualPlayerState& older,
    const VisualPlayerState& newer,
    VisualPlayerState& out,
    float lerpRatio
    ) {

    lerpSpecific(older.player1, newer.player1, out.player1, lerpRatio);
    lerpSpecific(older.player2, newer.player2, out.player2, lerpRatio);
}

void PlayerInterpolator::tick(float dt) {
    if (settings.realtime) return;

    for (auto& [playerId, player] : players) {
        if (player.totalFrames < 2) continue;

        float realFrameDelta = player.newerFrame.timestamp - player.olderFrame.timestamp;
        // this makes absolutely no sense im fucking fuming right now
        // why the fuck does a static non-changing number work better than an actual calculation
        float fakeFrameDelta = settings.expectedDelta;
        if (realFrameDelta == 0.f) {
            LerpLogger::get().logLerpSkip(playerId, this->getLocalTs(), player.timeCounter, player.interpolatedState.player1);
            continue;
        }

        float lerpRatio = (player.timeCounter - player.olderFrame.timestamp) / fakeFrameDelta;
        util::debugging::Benchmarker bb;

        lerpPlayer(player.olderFrame.visual, player.newerFrame.visual, player.interpolatedState, lerpRatio);

        LerpLogger::get().logLerpOperation(playerId, this->getLocalTs(), player.timeCounter, player.interpolatedState.player1);

        player.timeCounter += dt;
    }
}

VisualPlayerState& PlayerInterpolator::getPlayerState(uint32_t playerId) {
    return players.at(playerId).interpolatedState;
}

bool PlayerInterpolator::swapDeathStatus(uint32_t playerId) {
    auto& state = players.at(playerId);
    bool death = state.pendingDeath;
    state.pendingDeath = false;

    return death;
}

bool PlayerInterpolator::isPlayerStale(uint32_t playerId, float lastServerPacket) {
    auto uc = players.at(playerId).updateCounter;

    return uc != 0.f && !util::math::equal(uc, lastServerPacket);
}

float PlayerInterpolator::getLocalTs() {
    auto* gpl = static_cast<GlobedPlayLayer*>(PlayLayer::get());
    return gpl->m_fields->timeCounter;
}

PlayerInterpolator::LerpFrame::LerpFrame() {
    timestamp = 0.f;
    visual = {};
}

PlayerInterpolator::LerpFrame::LerpFrame(const PlayerData& data) {
    timestamp = data.timestamp;
    visual.player1 = data.player1;
    visual.player2 = data.player2;
}