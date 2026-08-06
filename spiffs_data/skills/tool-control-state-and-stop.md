# Tool Control State And Emergency Stop

Inspect or stop control_agent hardware actions with one immediate command.

## When to use

Use this skill when the user asks for:

- current control state, actuator state, busy state
- emergency stop, force stop, stop all hardware, IR stop
- 当前硬件状态、控制状态、急停、强制停止、停止硬件执行

## Single-tool rules

@rule trigger="控制状态|硬件状态|当前执行状态|control state|actuator state" target_role=control_agent action=control_state args={}
@rule trigger="急停|强制停止|停止硬件执行|停止所有硬件|emergency stop|force stop" target_role=control_agent action=control_emergency_stop args={}
@rule trigger="解除急停|清除急停|clear emergency stop" target_role=control_agent action=control_clear_emergency_stop args={}

## Safety boundary

- Emergency stop may be triggered immediately.
- Clearing emergency stop should only happen when the user clearly asks for it.
- Do not use skills to bypass Guardian policy or command queue validation.
