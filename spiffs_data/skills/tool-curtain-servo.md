# Tool Curtain Servo

Control the curtain simulator driven by the GPIO5 PWM servo on the control_agent.

## When to use

Use this skill when the user asks for:

- curtain, curtain opener, servo curtain
- open curtain, close curtain
- 窗帘、打开窗帘、关闭窗帘、拉开窗帘、拉上窗帘、舵机窗帘

## Single-tool rules

@rule trigger="打开窗帘|窗帘打开|开窗帘|拉开窗帘|open curtain|curtain open" target_role=control_agent action=set_curtain args={"state":"open"}
@rule trigger="关闭窗帘|窗帘关闭|关窗帘|拉上窗帘|close curtain|curtain close|curtain closed" target_role=control_agent action=set_curtain args={"state":"closed"}

## Notes

- Use set_curtain for normal open or closed requests.
- Use servo_write only when the user explicitly asks for an angle or pulse width.
- Current single-tool mode means "打开窗帘后再关闭" must be split into two user requests.
