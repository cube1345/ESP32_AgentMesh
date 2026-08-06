# Tool Status Light

Use the control_agent onboard WS2812 status light as the default visible actuator for demos and hardware validation.

## When to use

Use this skill when the user asks for:

- WS2812, status light, board light, RGB light, NeoPixel
- red, green, blue, white, yellow, purple, cyan, orange, or off
- 板载灯、状态灯、WS2812、彩灯、亮灯、关灯、红灯、蓝灯、绿灯

## Single-tool rules

These rules trigger one immediate Mesh command only. They do not create a workflow, cron job, or condition rule.

@rule trigger="红灯|红色状态灯|技能红灯|skill red|status red" target_role=control_agent action=set_status_light args={"color":"red"}
@rule trigger="蓝灯|蓝色状态灯|技能蓝灯|skill blue|status blue" target_role=control_agent action=set_status_light args={"color":"blue"}
@rule trigger="绿灯|绿色状态灯|技能绿灯|skill green|status green" target_role=control_agent action=set_status_light args={"color":"green"}
@rule trigger="白灯|白色状态灯|技能白灯|skill white|status white" target_role=control_agent action=set_status_light args={"color":"white"}
@rule trigger="黄灯|黄色状态灯|技能黄灯|skill yellow|status yellow" target_role=control_agent action=set_status_light args={"color":"yellow"}
@rule trigger="紫灯|紫色状态灯|技能紫灯|skill purple|status purple" target_role=control_agent action=set_status_light args={"color":"purple"}
@rule trigger="青灯|青色状态灯|技能青灯|skill cyan|status cyan" target_role=control_agent action=set_status_light args={"color":"cyan"}
@rule trigger="橙灯|橙色状态灯|技能橙灯|skill orange|status orange" target_role=control_agent action=set_status_light args={"color":"orange"}
@rule trigger="关灯|关闭状态灯|关闭板载灯|skill light off|status off" target_role=control_agent action=set_status_light args={"color":"off"}

## Safe wording

- Say "已命中 skill 规则" only after the rule path returns a Mesh dispatch result.
- If the control_agent result is delayed, say the command was dispatched and wait for the timeline result.
- Do not confuse this single-wire WS2812 with any removed three-wire RGB LED wording.
