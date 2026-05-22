#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_SCHEDULER_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_SCHEDULER_H_

namespace network_example::ai {

class AIScheduler {
public:
    explicit AIScheduler(float interval_seconds);

    bool update(float dt_seconds);
    void reset();

    float interval_seconds() const;
    float accumulated_seconds() const;

private:
    float interval_seconds_ = 0.0f;
    float accumulated_seconds_ = 0.0f;
};

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_AI_SCHEDULER_H_
