# Tool Runtime Skill Files

Manage Runtime Skills stored under /spiffs/skills.

## When to use

Use this skill when the user asks to view, write, update, or delete runtime skills.

## Rules

- Reading skills is low risk.
- Writing, editing, or deleting /spiffs/skills content requires explicit confirmation.
- A skill can contain stable facts, usage guidance, @match aliases, and one-shot @rule declarations.
- Skill rules can only trigger firmware-whitelisted actions; the firmware still validates target_role, action, and args.

## Recommended knowledge format

@match trigger="topic phrase|alias|entity name"

Put the facts that answer those topics below the match declaration. The
coordinator searches every normal user message against this metadata and only
injects the best one or two Runtime Skills.

## Recommended skill rule format

@rule trigger="phrase|alias" target_role=control_agent action=set_status_light args={"color":"blue"}

## Boundaries

- Do not place secrets or credentials in skills.
- Do not put long dynamic logs in skills.
- Keep each Runtime Skill small enough for MQTT update limits when using wireless upload.
- Keep aliases specific. Avoid generic entries such as help, status, or what.
