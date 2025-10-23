#include "app/Application.h"
#include "board/Board.h"

using namespace App;

static State state_ = State::Init;
static Mode mode_ = Mode::Boost;
static Targets targets_{};
static uint16_t duty_ = 0;
static PidController pidV;
static PidController pidI;

static inline Mode decide_mode(float vin, float vref) {
    if (vin < vref - 1.0f) return Mode::Boost;
    if (vin > vref + 1.0f) return Mode::Buck;
    return Mode::BuckBoost;
}

void App::Init() {
    HrtimDriver::Init(BoardCfg::kFsw_Hz, BoardCfg::kDeadtime_ns);
    AdcIf::InitAndStart();

    pidV.SetCoefficient(20, 0, 0, 0, 0).SetSaturation(-29800, 29800);
    pidI.SetCoefficient(15, 0, 0, 0, 0).SetSaturation(-29800, 29800);

    duty_ = HrtimDriver::PeriodTicks() / 2; // 50%
    HrtimDriver::SetDuty(BoardCfg::Leg::Boost, duty_);
    HrtimDriver::SetDuty(BoardCfg::Leg::Buck, duty_);

    state_ = State::WaitPd;
}

void App::SetTargets(const Targets &t) { targets_ = t; }

void App::OnPdContract(float v_V, float i_A) {
    targets_.vout_V = v_V;
    targets_.iout_A = i_A;
    if (state_ == State::WaitPd) state_ = State::SoftStart;
}

void App::OnControlTick() {
    if (state_ == State::Fault || state_ == State::Init) return;

    const float vin  = AdcIf::GetVin();
    const float vout = AdcIf::GetVout();
    const float iout = AdcIf::GetIout();

    mode_ = decide_mode(vin, targets_.vout_V);

    float result = 0.0f;
    switch (mode_) {
    case Mode::Boost:
        // Fix buck leg at safe duty, modulate boost leg
        HrtimDriver::SetDuty(BoardCfg::Leg::Buck, HrtimDriver::PeriodTicks() * 0.15f);
        pidV.SetReference(targets_.vout_V)
            .SetFeedback(vout, 1.0f / BoardCfg::kFsw_Hz)
            .Compute();
        result = pidV.Get();
        duty_ = static_cast<uint16_t>(std::min(std::max<int>(0, duty_ + static_cast<int>(result)), HrtimDriver::PeriodTicks()));
        HrtimDriver::SetDuty(BoardCfg::Leg::Boost, duty_);
        break;
    case Mode::BuckBoost:
        // Share effort; hold boost at low duty, regulate with buck
        HrtimDriver::SetDuty(BoardCfg::Leg::Boost, HrtimDriver::PeriodTicks() * 0.3f);
        pidV.SetReference(targets_.vout_V)
            .SetFeedback(vout, 1.0f / BoardCfg::kFsw_Hz)
            .Compute();
        result = pidV.Get();
        duty_ = static_cast<uint16_t>(std::min(std::max<int>(0, duty_ + static_cast<int>(result)), HrtimDriver::PeriodTicks()));
        HrtimDriver::SetDuty(BoardCfg::Leg::Buck, duty_);
        break;
    case Mode::Buck:
        // Fix boost leg at minimum, regulate with buck
        HrtimDriver::SetDuty(BoardCfg::Leg::Boost, HrtimDriver::PeriodTicks() * 0.2f);
        pidV.SetReference(targets_.vout_V)
            .SetFeedback(vout, 1.0f / BoardCfg::kFsw_Hz)
            .Compute();
        result = pidV.Get();
        duty_ = static_cast<uint16_t>(std::min(std::max<int>(0, duty_ + static_cast<int>(result)), HrtimDriver::PeriodTicks()));
        HrtimDriver::SetDuty(BoardCfg::Leg::Buck, duty_);
        break;
    }
}

void App::OnLowTick() {
    // Safety checks placeholders (UVLO/OVLO/OCP/OTP)
    const float vin  = AdcIf::GetVin();
    const float vout = AdcIf::GetVout();
    (void)vin; (void)vout;
}
