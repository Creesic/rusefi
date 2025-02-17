#include "pch.h"

#include "rusefi_lua.h"
#include "lua_hooks.h"

#include "fuel_math.h"
#include "airmass.h"
#include "lua_airmass.h"
#include "value_lookup.h"
#if EFI_CAN_SUPPORT || EFI_UNIT_TEST
#include "can_msg_tx.h"
#endif // EFI_CAN_SUPPORT
#include "crc.h"
#include "settings.h"
#include <new>

// We don't want to try and use the STL on a microcontroller
#define LUAAA_WITHOUT_CPP_STDLIB
#include "luaaa.hpp"
#include "lua_hooks_util.h"
using namespace luaaa;

// Some functions lean on existing FSIO implementation
#include "fsio_impl.h"

#if EFI_PROD_CODE
#include "electronic_throttle_impl.h"
#endif

static int lua_readpin(lua_State* l) {
	auto msg = luaL_checkstring(l, 1);
#if EFI_PROD_CODE
	brain_pin_e pin = parseBrainPin(msg);
	if (!isBrainPinValid(pin)) {
		lua_pushnil(l);
	} else {
		int physicalValue = palReadPad(getHwPort("read", pin), getHwPin("read", pin));
		lua_pushnumber(l, physicalValue);
	}
#endif
	return 1;
}

static int getSensor(lua_State* l, SensorType type) {
	auto result = Sensor::get(type);

	if (result) {
		// return value if valid
		lua_pushnumber(l, result.Value);
	} else {
		// return nil if invalid
		lua_pushnil(l);
	}

	return 1;
}

static int lua_getAuxAnalog(lua_State* l) {
	// todo: shall we use HUMAN_INDEX since UI goes from 1 and Lua loves going from 1?
	auto zeroBasedSensorIndex = luaL_checkinteger(l, 1);

	auto type = static_cast<SensorType>(zeroBasedSensorIndex + static_cast<int>(SensorType::Aux1));

	return getSensor(l, type);
}

static int lua_getSensorByIndex(lua_State* l) {
	auto zeroBasedSensorIndex = luaL_checkinteger(l, 1);

	return getSensor(l, static_cast<SensorType>(zeroBasedSensorIndex));
}

static int lua_getSensorByName(lua_State* l) {
	auto sensorName = luaL_checklstring(l, 1, nullptr);
	SensorType type = findSensorTypeByName(sensorName);

	return getSensor(l, type);
}

static int lua_getSensorRaw(lua_State* l) {
	auto zeroBasedSensorIndex = luaL_checkinteger(l, 1);

	lua_pushnumber(l, Sensor::getRaw(static_cast<SensorType>(zeroBasedSensorIndex)));
	return 1;
}

static int lua_hasSensor(lua_State* l) {
	auto zeroBasedSensorIndex = luaL_checkinteger(l, 1);

	lua_pushboolean(l, Sensor::hasSensor(static_cast<SensorType>(zeroBasedSensorIndex)));
	return 1;
}

static int lua_table3d(lua_State* l) {
	auto humanTableIdx = luaL_checkinteger(l, 1);
	auto x = luaL_checknumber(l, 2);
	auto y = luaL_checknumber(l, 3);

	// index table, compute table lookup
	auto result = getscriptTable(humanTableIdx - HUMAN_OFFSET)->getValue(x, y);

	lua_pushnumber(l, result);
	return 1;
}

static int lua_curve2d(lua_State* l) {
	// index starting from 1
	auto humanCurveIdx = luaL_checkinteger(l, 1);
	auto x = luaL_checknumber(l, 2);

	auto result = getCurveValue(humanCurveIdx - HUMAN_OFFSET, x);

	lua_pushnumber(l, result);
	return 1;
}

static int lua_findCurveIndex(lua_State* l) {
	auto name = luaL_checklstring(l, 1, nullptr);
	auto result = getCurveIndexByName(name);
	if (result == EFI_ERROR_CODE) {
		lua_pushnil(l);
	} else {
		// TS counts curve from 1 so convert indexing here
		lua_pushnumber(l, result + HUMAN_OFFSET);
	}
	return 1;
}

static uint32_t getArray(lua_State* l, int paramIndex, uint8_t *data, uint32_t size) {
	uint32_t result = 0;

	luaL_checktype(l, paramIndex, LUA_TTABLE);
	while (true) {
		lua_pushnumber(l, result + 1);
		auto elementType = lua_gettable(l, paramIndex);
		auto val = lua_tonumber(l, -1);
		lua_pop(l, 1);

		if (elementType == LUA_TNIL) {
			// we're done, this is the end of the array.
			break;
		}

		if (elementType != LUA_TNUMBER) {
			// We're not at the end, but this isn't a number!
			luaL_error(l, "Unexpected data at position %d: %s", result, lua_tostring(l, -1));
		}

		// This element is valid, increment DLC
		result++;

		if (result > size) {
			luaL_error(l, "Input array longer than buffer");
		}

		data[result - 1] = val;
	}
	return result;
}

#if EFI_CAN_SUPPORT || EFI_UNIT_TEST
static int lua_txCan(lua_State* l) {
	auto channel = luaL_checkinteger(l, 1);
	// TODO: support multiple channels
	luaL_argcheck(l, channel == 1 || channel == 2, 1, "only channels 1 and 2 currently supported");

	auto id = luaL_checkinteger(l, 2);
	auto ext = luaL_checkinteger(l, 3);

	// Check that ID is valid based on std vs. ext
	if (ext == 0) {
		luaL_argcheck(l, id <= 0x7FF, 2, "ID specified is greater than max std ID");
	} else {
		luaL_argcheck(l, id <= 0x1FFF'FFFF, 2, "ID specified is greater than max ext ID");
	}

	// conform ext parameter to true/false
	CanTxMessage msg(CanCategory::LUA, id, 8, ext == 0 ? false : true);
	msg.busIndex = channel - HUMAN_OFFSET;

	// Unfortunately there is no way to inspect the length of a table,
	// so we have to just iterate until we run out of numbers
	uint8_t dlc = 0;

	// todo: reduce code duplication with getArray
	luaL_checktype(l, 4, LUA_TTABLE);
	while (true) {
		lua_pushnumber(l, dlc + 1);
		auto elementType = lua_gettable(l, 4);
		auto val = lua_tonumber(l, -1);
		lua_pop(l, 1);

		if (elementType == LUA_TNIL) {
			// we're done, this is the end of the array.
			break;
		}

		if (elementType != LUA_TNUMBER) {
			// We're not at the end, but this isn't a number!
			luaL_error(l, "Unexpected CAN data at position %d: %s", dlc, lua_tostring(l, -1));
		}

		// This element is valid, increment DLC
		dlc++;

		if (dlc > 8) {
			luaL_error(l, "CAN frame length cannot be longer than 8");
		}

		msg[dlc - 1] = val;
	}

	msg.setDlc(dlc);

	// no return value
	return 0;
}
#endif // EFI_CAN_SUPPORT

static LuaAirmass luaAirmass;

AirmassModelBase& getLuaAirmassModel() {
	return luaAirmass;
}

#if !EFI_UNIT_TEST
static SimplePwm pwms[LUA_PWM_COUNT];
static OutputPin pins[LUA_PWM_COUNT];

struct P {
	SimplePwm& pwm;
	lua_Integer idx;
};

static P luaL_checkPwmIndex(lua_State* l, int pos) {
	auto channel = luaL_checkinteger(l, pos);

	// Ensure channel is valid
	if (channel < 0 || channel >= LUA_PWM_COUNT) {
		luaL_error(l, "setPwmDuty invalid channel %d", channel);
	}

	return { pwms[channel], channel };
}

static int lua_startPwm(lua_State* l) {
	auto p = luaL_checkPwmIndex(l, 1);
	auto freq = luaL_checknumber(l, 2);
	auto duty = luaL_checknumber(l, 3);

	// clamp to 1..1000 hz
	freq = clampF(1, freq, 1000);

	startSimplePwmExt(
		&p.pwm, "lua", &engine->executor,
		engineConfiguration->luaOutputPins[p.idx], &pins[p.idx],
		freq, duty
	);

	return 0;
}

void luaDeInitPins() {
	// Simply de-init all pins - when the script runs again, they will be re-init'd
	for (size_t i = 0; i < efi::size(pins); i++) {
		pins[i].deInit();
	}
}

static int lua_setPwmDuty(lua_State* l) {
	auto p = luaL_checkPwmIndex(l, 1);
	auto duty = luaL_checknumber(l, 2);

	// clamp to 0..1
	duty = clampF(0, duty, 1);

	p.pwm.setSimplePwmDutyCycle(duty);

	return 0;
}

static int lua_setPwmFreq(lua_State* l) {
	auto p = luaL_checkPwmIndex(l, 1);
	auto freq = luaL_checknumber(l, 2);

	// clamp to 1..1000 hz
	freq = clampF(1, freq, 1000);

	p.pwm.setFrequency(freq);

	return 0;
}

static int lua_fan(lua_State* l) {
	lua_pushboolean(l, enginePins.fanRelay.getLogicValue());
	return 1;
}

static int lua_getDigital(lua_State* l) {
	auto idx = luaL_checkinteger(l, 1);

	bool state = false;

	switch (idx) {
		case 0: state = engine->engineState.clutchDownState; break;
		case 1: state = engine->clutchUpState; break;
		case 2: state = engine->brakePedalState; break;
		case 3: state = engine->module<AcController>().unmock().acButtonState; break;
		default:
			// Return nil to indicate invalid parameter
			lua_pushnil(l);
			return 1;
	}

	lua_pushboolean(l, state);
	return 1;
}

static int lua_setDebug(lua_State* l) {
	// wrong debug mode, ignore
	if (engineConfiguration->debugMode != DBG_LUA) {
		return 0;
	}

	auto idx = luaL_checkinteger(l, 1);
	auto val = luaL_checknumber(l, 2);

	// invalid index, ignore
	if (idx < 1 || idx > 7) {
		return 0;
	}

	auto firstDebugField = &engine->outputChannels.debugFloatField1;
	firstDebugField[idx - 1] = val;

	return 0;
}

static auto lua_getAirmassResolveMode(lua_State* l) {
	if (lua_gettop(l) == 0) {
		// zero args, return configured mode
		return engineConfiguration->fuelAlgorithm;
	} else {
		return static_cast<engine_load_mode_e>(luaL_checkinteger(l, 1));
	}
}

static int lua_getAirmass(lua_State* l) {
	auto airmassMode = lua_getAirmassResolveMode(l);
	auto airmass = getAirmassModel(airmassMode);

	if (!airmass) {
		return luaL_error(l, "null airmass");
	}

	auto rpm = Sensor::getOrZero(SensorType::Rpm);
	auto result = airmass->getAirmass(rpm).CylinderAirmass;

	lua_pushnumber(l, result);
	return 1;
}

static int lua_setAirmass(lua_State* l) {
	float airmass = luaL_checknumber(l, 1);
	float engineLoadPercent = luaL_checknumber(l, 2);

	airmass = clampF(0, airmass, 10);
	engineLoadPercent = clampF(0, engineLoadPercent, 1000);

	luaAirmass.setAirmass({airmass, engineLoadPercent});

	return 0;
}

#endif // EFI_UNIT_TEST

struct LuaSensor final : public StoredValueSensor {
	LuaSensor() : LuaSensor("Invalid") { }

	~LuaSensor() {
		unregister();
	}

	LuaSensor(const char* name)
		: StoredValueSensor(findSensorTypeByName(name), MS2NT(100))
	{
		Register();
	}

	void set(float value) {
		setValidValue(value, getTimeNowNt());
	}

	void invalidate() {
		StoredValueSensor::invalidate();
	}

	void showInfo(const char*) const {}
};

struct LuaPid final {
	LuaPid() = default;

	LuaPid(float kp, float ki, float kd, float min, float max)
		: m_pid(&m_params)
	{
		m_params.pFactor = kp;
		m_params.iFactor = ki;
		m_params.dFactor = kd;

		m_params.offset = 0;
		m_params.periodMs = 0;
		m_params.minValue = min;
		m_params.maxValue = max;

		m_lastUpdate.reset();
	}

	float get(float target, float input) {
#if EFI_UNIT_TEST
		extern int timeNowUs;
		// this is how we avoid zero dt
		timeNowUs += 1000;
#endif

		float dt = m_lastUpdate.getElapsedSecondsAndReset(getTimeNowNt());

		return m_pid.getOutput(target, input, dt);
	}

	void reset() {
		m_pid.reset();
	}

private:
	Pid m_pid;
	Timer m_lastUpdate;
	pid_s m_params;
};

void configureRusefiLuaHooks(lua_State* l) {
	LuaClass<Timer> luaTimer(l, "Timer");
	luaTimer
		.ctor()
		.fun("reset",             static_cast<void (Timer::*)()     >(&Timer::reset            ))
		.fun("getElapsedSeconds", static_cast<float(Timer::*)()const>(&Timer::getElapsedSeconds));

	LuaClass<LuaSensor> luaSensor(l, "Sensor");
	luaSensor
		.ctor<const char*>()
		.fun("set", &LuaSensor::set)
		.fun("invalidate", &LuaSensor::invalidate);

	LuaClass<LuaPid> luaPid(l, "Pid");
	luaPid
		.ctor<float, float, float, float, float>()
		.fun("get", &LuaPid::get)
		.fun("reset", &LuaPid::reset);

	configureRusefiLuaUtilHooks(l);

	lua_register(l, "readPin", lua_readpin);
	lua_register(l, "getAuxAnalog", lua_getAuxAnalog);
	lua_register(l, "getSensorByIndex", lua_getSensorByIndex);
	lua_register(l, "getSensor", lua_getSensorByName);
	lua_register(l, "getSensorRaw", lua_getSensorRaw);
	lua_register(l, "hasSensor", lua_hasSensor);
	lua_register(l, "table3d", lua_table3d);
	lua_register(l, "curve", lua_curve2d);
	lua_register(l, "findCurveIndex", lua_findCurveIndex);

#if EFI_CAN_SUPPORT || EFI_UNIT_TEST
	lua_register(l, "txCan", lua_txCan);
#endif

	lua_register(l, "findTableIndex",
			[](lua_State* l) {
			auto name = luaL_checklstring(l, 1, nullptr);
			auto index = getTableIndexByName(name);
			if (index == EFI_ERROR_CODE) {
				lua_pushnil(l);
			} else {
				// TS counts curve from 1 so convert indexing here
				lua_pushnumber(l, index + HUMAN_OFFSET);
			}
			return 1;
	});

	lua_register(l, "findSetting",
			[](lua_State* l) {
			auto name = luaL_checklstring(l, 1, nullptr);
			auto defaultValue = luaL_checknumber(l, 2);

			auto index = getSettingIndexByName(name);
			if (index == EFI_ERROR_CODE) {
				lua_pushnumber(l, defaultValue);
			} else {
				// TS counts curve from 1 so convert indexing here
				lua_pushnumber(l, engineConfiguration->scriptSetting[index]);
			}
			return 1;
	});

#if EFI_LAUNCH_CONTROL
	lua_register(l, "setSparkSkipRatio", [](lua_State* l) {
		auto targetSkipRatio = luaL_checknumber(l, 1);
		engine->softSparkLimiter.setTargetSkipRatio(targetSkipRatio);
		return 1;
	});
#endif // EFI_LAUNCH_CONTROL

	lua_register(l, "enableCanTx", [](lua_State* l) {
		engine->allowCanTx = lua_toboolean(l, 1);
		return 0;
	});

	lua_register(l, "crc8_j1850", [](lua_State* l) {
		uint8_t data[8];
		uint32_t length = getArray(l, 1, data, sizeof(data));
		auto trimLength = luaL_checkinteger(l, 2);
		int crc = crc8(data, minI(length, trimLength));

		lua_pushnumber(l, crc);
		return 1;
	});


	lua_register(l, "setTimingMult", [](lua_State* l) {
		engine->ignitionState.luaTimingMult = luaL_checknumber(l, 1);
		return 0;
	});
	lua_register(l, "setFuelAdd", [](lua_State* l) {
		engine->engineState.lua.fuelAdd = luaL_checknumber(l, 1);
		return 0;
	});
	lua_register(l, "setFuelMult", [](lua_State* l) {
		engine->engineState.lua.fuelMult = luaL_checknumber(l, 1);
		return 0;
	});
#if EFI_PROD_CODE
	lua_register(l, "setEtbAdd", [](lua_State* l) {
		auto luaAdjustment = luaL_checknumber(l, 1);
		for (int i = 0 ; i < ETB_COUNT; i++) {
			extern EtbController* etbControllers[];
			etbControllers[i]->luaAdjustment = luaAdjustment;
		}
		return 0;
	});
#endif // EFI_PROD_CODE

	lua_register(l, "setClutchUpState", [](lua_State* l) {
		engine->engineState.lua.clutchUpState = lua_toboolean(l, 1);
		return 0;
	});

	lua_register(l, "setBrakePedalState", [](lua_State* l) {
		engine->engineState.lua.brakePedalState = lua_toboolean(l, 1);
		return 0;
	});

	lua_register(l, "getCalibration", [](lua_State* l) {
		auto propertyName = luaL_checklstring(l, 1, nullptr);
		auto result = getConfigValueByName(propertyName);
		lua_pushnumber(l, result);
		return 1;
	});

	lua_register(l, "getOutput", [](lua_State* l) {
		auto propertyName = luaL_checklstring(l, 1, nullptr);
		auto result = getOutputValueByName(propertyName);
		lua_pushnumber(l, result);
		return 1;
	});

#if EFI_SHAFT_POSITION_INPUT
	lua_register(l, "getEngineState", [](lua_State* l) {
		spinning_state_e state = engine->rpmCalculator.getState();
		int luaStateCode;
		if (state == STOPPED) {
			luaStateCode = 0;
		} else if (state == RUNNING) {
			luaStateCode = 2;
		} else {
			// spinning-up or cranking
			luaStateCode = 1;
		}
		lua_pushnumber(l, luaStateCode);
		return 1;
	});
#endif //EFI_SHAFT_POSITION_INPUT

	lua_register(l, "setCalibration", [](lua_State* l) {
		auto propertyName = luaL_checklstring(l, 1, nullptr);
		auto value = luaL_checknumber(l, 2);
		auto incrementVersion = lua_toboolean(l, 3);
		setConfigValueByName(propertyName, value);
		if (incrementVersion) {
			incrementGlobalConfigurationVersion();
		}
		return 0;
	});

	lua_register(l, "setAcDisabled", [](lua_State* l) {
		auto value = lua_toboolean(l, 1);
		engine->module<AcController>().unmock().isDisabledByLua = value;
		return 0;
	});
	lua_register(l, "getTimeSinceAcToggleMs", [](lua_State* l) {
		int result = US2MS(getTimeNowUs()) - engine->module<AcController>().unmock().acSwitchLastChangeTimeMs;
		lua_pushnumber(l, result);
		return 1;
	});

	lua_register(l, "setTimingAdd", [](lua_State* l) {
		engine->ignitionState.luaTimingAdd = luaL_checknumber(l, 1);
		return 0;
	});

#if !EFI_UNIT_TEST
	lua_register(l, "startPwm", lua_startPwm);
	lua_register(l, "setPwmDuty", lua_setPwmDuty);
	lua_register(l, "setPwmFreq", lua_setPwmFreq);

	lua_register(l, "getFan", lua_fan);
	lua_register(l, "getDigital", lua_getDigital);
	lua_register(l, "setDebug", lua_setDebug);
	lua_register(l, "getAirmass", lua_getAirmass);
	lua_register(l, "setAirmass", lua_setAirmass);

	lua_register(l, "stopEngine", [](lua_State*) {
		doScheduleStopEngine();
		return 0;
	});

	lua_register(l, "getTimeSinceTriggerEventMs", [](lua_State* l) {
		int result = engine->triggerCentral.m_lastEventTimer.getElapsedUs() / 1000;
		lua_pushnumber(l, result);
		return 1;
	});


#if EFI_CAN_SUPPORT
	lua_register(l, "canRxAdd", [](lua_State* l) {
		auto eid = luaL_checkinteger(l, 1);
		addLuaCanRxFilter(eid);

		return 0;
	});
#endif // EFI_CAN_SUPPORT
#endif // not EFI_UNIT_TEST
}
