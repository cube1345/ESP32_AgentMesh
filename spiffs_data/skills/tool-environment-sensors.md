# Tool Environment Sensors

Route environment sensing requests to the sensor_agent through Mesh.

## When to use

Use this skill when the user asks for:

- temperature, humidity, AHT20, AHT10
- air quality, SGP30, TVOC, eCO2, VOC
- light level, illuminance, lux, GY-30, BH1750
- 温度、湿度、温湿度、空气质量、光照、照度、勒克斯、环境数据、综合传感器

## Single-tool rules

@rule trigger="读取温湿度|温湿度|湿度多少|温度多少|read humidity|read temperature" target_role=sensor_agent action=read_temperature_humidity args={}
@rule trigger="读取环境数据|环境数据|综合传感器|全部传感器|read environment" target_role=sensor_agent action=read_environment args={}
@rule trigger="读取光照|光照强度|照度|勒克斯|lux|read light|light level" target_role=sensor_agent action=read_light_level args={}

## Notes

- Ordinary Feishu environment requests should not read the coordinator local I2C sensor.
- Route through sensor_agent unless the user explicitly says local board or local I2C diagnostics.
- Do not create a condition rule from a threshold sentence in the current firmware mode; ask for one immediate action instead.
