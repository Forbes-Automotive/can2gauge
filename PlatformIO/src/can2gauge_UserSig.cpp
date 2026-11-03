#include <can2gauge_UserSig.h>
#include <can2gauge_CanMon.h>
#include <can2gauge_ELM.h>
#include <can2gauge_ECU.h> // ECU channels over WiFi from usb2ip (USRC_ECU)
#include <can2gauge_VagDiag.h> // VAG diag values (USRC_DIAG): their state for the note under the gauge
#include <CANLine.h>
#include <ME7Logger.h> // 0.17.6: ME7 logger variables (USRC_ME7): the logger's state and each variable's scaling

// per user signal: where its DBC signal sits (looked up again when the DBC changes) and the last frame decoded
static int32_t dbcSignalIndex[USER_SIGS_ALL];    // index into the DBC's signals, -1 = not found
static uint32_t lookupGeneration[USER_SIGS_ALL]; // the DBC generation the index belongs to
static uint8_t dbcDecimals[USER_SIGS_ALL];       // 0.36.14: the DBC signal's decimals, from its factor (found with the index)
static uint32_t lastFrameMs[USER_SIGS_ALL];

// 0.36.14: how many decimals a value scaled as raw x scale + offset has: a whole scale and offset = none, 0.1 = one,
// finer = two (the most the gauge shows). The same rule as the CAN tab's DBC view (canDbcFormat).
static uint8_t decimalsOfScale(float scale, float offset)
{
  const float step = fabsf(scale); // how much one count of the raw value is worth
  const bool isWholeScale = fabsf(scale - lroundf(scale)) < 1e-6f;
  const bool isWholeOffset = fabsf(offset - lroundf(offset)) < 1e-6f;
  if (step >= 1.0f && isWholeScale && isWholeOffset)
  {
    return 0; // whole steps: 850 rpm, 90 C
  }
  if (step >= 0.1f)
  {
    return 1; // 0.1 steps: 13.8 V
  }
  return 2; // finer: 0.98 lambda
}

// user signal userSigIndex: 0..15 = page main / second values (page = index / 2), 16..39 = page slots (Quad values 2-4,
// Custom centre stack: page = (index - 16) / 3)
static const UserSigCfg &configOf(uint8_t userSigIndex)
{
  if (userSigIndex < USER_SIGS)
  {
    return settings.pages[userSigIndex / 2].valueSigs[userSigIndex % 2];
  }
  const uint8_t slotIndex = userSigIndex - USER_SIGS;
  return settings.pages[slotIndex / 3].slotSigs[slotIndex % 3];
}

static uint32_t applyGeneration = 1; // bumped by userSigApply so every lookup is redone

const UserSigCfg *userSigConfig(signalId_t id)
{
  if (id < SIG_USER_1 || id >= SIG_USER_1 + USER_SIGS_ALL)
  {
    return NULL;
  }
  const UserSigCfg &userSig = configOf(id - SIG_USER_1);
  return userSig.source == USRC_NONE ? NULL : &userSig;
}

bool userSigPageUses(uint8_t cfgIndex, uint8_t source)
{
  if (cfgIndex >= MAX_PAGES)
  {
    return false;
  }
  const GaugePageCfg &page = settings.pages[cfgIndex];
  const bool slots = page.style == STYLE_QUAD || page.style == STYLE_TRIPLE || page.style == STYLE_CUSTOM;
  for (uint8_t id : {page.signal, page.signal2, slots ? settings.pages[cfgIndex].slots[0] : (uint8_t)SIG_NONE,
                     slots ? settings.pages[cfgIndex].slots[1] : (uint8_t)SIG_NONE,
                     slots ? settings.pages[cfgIndex].slots[2] : (uint8_t)SIG_NONE})
  {
    const UserSigCfg *userSig = userSigConfig((signalId_t)id);
    if (userSig && userSig->source == source)
    {
      return true;
    }
  }
  return false;
}

// 0.17.6: does an enabled page show user signal userSigIndex (as its main / second value, or a Quad / Custom slot)?
// The ME7 logger logs only the variables pages in the rotation actually show.
bool userSigInRotation(uint8_t userSigIndex)
{
  const uint8_t id = SIG_USER_1 + userSigIndex;
  for (uint8_t cfgIndex = 0; cfgIndex < MAX_PAGES; cfgIndex++)
  {
    const GaugePageCfg &page = settings.pages[cfgIndex];
    if (!page.enabled)
    {
      continue;
    }
    if (page.signal == id || page.signal2 == id)
    {
      return true;
    }
    if (page.style == STYLE_QUAD || page.style == STYLE_TRIPLE || page.style == STYLE_CUSTOM)
    {
      for (uint8_t slot = 0; slot < 3; slot++)
      {
        if (settings.pages[cfgIndex].slots[slot] == id)
        {
          return true;
        }
      }
    }
  }
  return false;
}

// 0.35.0: does a switched-on trigger watch user signal userSigIndex? A trigger on a page value needs it read whether or
// not its page is showing (or even switched on), or the trigger could never fire.
bool userSigOnTrigger(uint8_t userSigIndex)
{
  const uint8_t id = SIG_USER_1 + userSigIndex;
  for (uint8_t triggerIndex = 0; triggerIndex < MAX_TRIGGERS; triggerIndex++)
  {
    if (settings.triggers[triggerIndex].enabled && settings.triggers[triggerIndex].signal == id)
    {
      return true;
    }
  }
  return false;
}

// 0.35.0: should this user signal's source keep reading it? Yes when an enabled page shows it or a trigger watches it.
bool userSigWanted(uint8_t userSigIndex)
{
  if (userSigInRotation(userSigIndex))
  {
    return true;
  }
  return userSigOnTrigger(userSigIndex);
}

// 0.37.0: is VAG diag value number valueIndex + 1 shown on an enabled page, or watched by a switched-on trigger? The
// VAG diag task only asks for the values something uses (they can come from several modules now)
bool userSigDiagValueWanted(uint8_t valueIndex)
{
  for (uint8_t userSigIndex = 0; userSigIndex < USER_SIGS_ALL; userSigIndex++)
  {
    const UserSigCfg &userSig = configOf(userSigIndex);
    if (userSig.source == USRC_DIAG && userSig.diagValue == valueIndex + 1 && userSigWanted(userSigIndex))
    {
      return true;
    }
  }
  return false;
}

// any page in the rotation (enabled in the web UI) reading from this source - the sources that only connect on demand
// use this to connect at power-up instead, so the value is already there when its page is swiped to.
// 0.35.0: a switched-on trigger watching a value from this source counts too, so the trigger's value keeps coming.
bool userSigRotationUses(uint8_t source)
{
  for (uint8_t cfgIndex = 0; cfgIndex < MAX_PAGES; cfgIndex++)
  {
    if (settings.pages[cfgIndex].enabled && userSigPageUses(cfgIndex, source))
    {
      return true;
    }
  }
  for (uint8_t triggerIndex = 0; triggerIndex < MAX_TRIGGERS; triggerIndex++)
  {
    const TriggerCfg &trigger = settings.triggers[triggerIndex];
    if (!trigger.enabled)
    {
      continue;
    }
    const UserSigCfg *userSigPointer = userSigConfig((signalId_t)trigger.signal);
    if (userSigPointer == NULL)
    {
      continue; // one of the gauge's own signals, or not a set-up page value
    }
    const UserSigCfg &userSig = *userSigPointer; // the page value's settings, read with dots
    if (userSig.source == source)
    {
      return true;
    }
  }
  return false;
}

// 0.35.0: the OBD PIDs the switched-on triggers watch, added to the list so the OBD polling asks for them even when
// their page isn't on screen (a PID with no gauge signal of its own is only asked for while a page shows it)
uint8_t userSigTriggerPids(uint8_t *pids, uint8_t count, uint8_t maxPids)
{
  for (uint8_t triggerIndex = 0; triggerIndex < MAX_TRIGGERS && count < maxPids; triggerIndex++)
  {
    const TriggerCfg &trigger = settings.triggers[triggerIndex];
    if (!trigger.enabled)
    {
      continue;
    }
    const UserSigCfg *userSigPointer = userSigConfig((signalId_t)trigger.signal);
    if (userSigPointer == NULL)
    {
      continue; // a gauge signal (rpm, coolant ...): its PID is asked for all the time anyway
    }
    const UserSigCfg &userSig = *userSigPointer; // the page value's settings, read with dots
    if (userSig.source != USRC_OBD || userSig.pid == 0)
    {
      continue;
    }
    bool isListed = false; // already on the list (the page on screen shows it too)
    for (uint8_t i = 0; i < count; i++)
    {
      if (pids[i] == userSig.pid)
      {
        isListed = true;
      }
    }
    if (!isListed)
    {
      pids[count] = userSig.pid;
      count++;
    }
  }
  return count;
}

// true while the value's source is still opening its connection, so the page can show a spinner beside the note.
// False once connected (even if the value hasn't come yet), and false when nothing is going to connect (nothing paired,
// switched off, not set up): the note under the value says why instead.
bool userSigConnecting(signalId_t id)
{
  const UserSigCfg *userSigPointer = userSigConfig(id);
  if (userSigPointer == NULL)
  {
    return false; // one of the gauge's own signals
  }
  const UserSigCfg &userSig = *userSigPointer; // the page value's settings, read with dots
  if (userSig.source == USRC_KLINE)
  {
    if (runtime.klinePage.connected && runtime.klinePage.addr == userSig.klineAddr)
    {
      return false; // the session to its module is open
    }
    if (settings.klineProtocol == KL_ME7LOGGER)
    {
      return false; // 0.17.6: the ME7 logger keeps the line, nothing is connecting for this page (the note says so)
    }
    return true; // 5-baud init, retrying, or waiting for the line
  }
  if (userSig.source == USRC_ME7) // 0.17.6: spins while the ME7 logger loads its files, wakes the ECU and sets up
  {
    if (settings.klineProtocol != KL_ME7LOGGER)
    {
      return false; // the logger isn't picked: the note says so
    }
    const me7State_t loggerState = me7State();
    return loggerState == ME7_LOADING || loggerState == ME7_CONNECTING || loggerState == ME7_DEFINING;
  }
  if (userSig.source == USRC_OBD)
  {
    if ((settings.canProfile == CAN_PROFILE_OBD2 || settings.obdOverCan) && canStats().up)
    {
      return false; // OBD-II on the wire: nothing to connect
    }
    if (settings.elmTransport == ELM_TR_WIFI && !settings.elmWifiSsid[0])
    {
      return false; // no WiFi adapter set up
    }
    if (settings.elmTransport != ELM_TR_WIFI && !settings.elmMac[0])
    {
      return false; // no Bluetooth adapter paired: the note says so
    }
    if (elmConnected() && !strcmp(elmStatus().state, "polling"))
    {
      return false; // the adapter is up
    }
    return true;
  }
  if (userSig.source == USRC_DIAG)
  {
    if (settings.vdiagRun == VDIAG_RUN_OFF || userSig.diagValue < 1 || userSig.diagValue > VDIAG_VALUES ||
        settings.vdiagValues[userSig.diagValue - 1].kind == VDIAG_KIND_NONE)
    {
      return false; // switched off or not set up: the note says so
    }
    const VdiagStatus &diag = vdiagStatus();
    if (!diag.values[userSig.diagValue - 1].supported)
    {
      return false; // the module refused it
    }
    if (!strcmp(diag.state, "polling"))
    {
      return false; // talking to the module
    }
    return true;
  }
  return false; // CAN, ECU over WiFi: nothing to connect
}

// "°C" / "degC" -> "C" so the imperial option converts it like the gauge's own temperatures
static void normaliseDbcUnit(const char *dbcUnit, char *out, size_t len)
{
  if (!strcmp(dbcUnit, "\xC2\xB0"
                       "C") ||
      !strcmp(dbcUnit, "degC"))
  {
    copyStr(out, len, "C");
  }
  else if (!strcmp(dbcUnit, "rpm") || !strcmp(dbcUnit, "U/min"))
  {
    copyStr(out, len, "rpm");
  }
  else
  {
    copyStr(out, len, dbcUnit);
  }
}

void userSigApply()
{
  applyGeneration++;
  for (uint8_t userSigIndex = 0; userSigIndex < USER_SIGS_ALL; userSigIndex++)
  {
    const UserSigCfg &userSig = configOf(userSigIndex);
    const GaugePageCfg &page = settings.pages[userSigIndex < USER_SIGS ? userSigIndex / 2 : (userSigIndex - USER_SIGS) / 3];
    const signalId_t id = (signalId_t)(SIG_USER_1 + userSigIndex);
    char unit[10] = "";
    char name[24] = "";
    if (userSig.source == USRC_KLINE) // the K-line task renames it from the module's label file once connected
    {
      snprintf(name, sizeof(name), "Block %u field %u", userSig.klineGroup, userSig.klineField + 1);
    }
    else if (userSig.source == USRC_CAN_DBC)
    {
      copyStr(name, sizeof(name), userSig.sigName);
    }
    else if (userSig.source == USRC_ME7) // 0.17.6: the .ecu name ("nmot_w"); the K-line task adds the unit once loaded
    {
      copyStr(name, sizeof(name), userSig.sigName);
    }
    else if (userSig.source == USRC_ECU) // the channel's name and unit from the ECU's channel list
    {
      EcuChannelInfo info;
      if (ecuNowChannelInfo(userSig.ecuKind, userSig.ecuChannel, info))
      {
        copyStr(name, sizeof(name), info.name);
        ecuNowGaugeUnit(info.unit, unit, sizeof(unit));
      }
      else
      {
        snprintf(name, sizeof(name), "Channel %u", userSig.ecuChannel);
      }
    }
    else if (userSig.source == USRC_DIAG && userSig.diagValue >= 1 && userSig.diagValue <= VDIAG_VALUES) // the VAG diag tab's name
    {
      const VdiagValue &vdiagValue = settings.vdiagValues[userSig.diagValue - 1];
      if (vdiagValue.name[0])
      {
        copyStr(name, sizeof(name), vdiagValue.name);
      }
      else
      {
        snprintf(name, sizeof(name), "Diag value %u", userSig.diagValue);
      }
      copyStr(unit, sizeof(unit), vdiagValue.unit); // a KWP value with no unit typed gets its formula's unit when read
    }
    if (userSig.source == USRC_OBD) // the PID's name and unit from the shared OBD table (CANLine)
    {
      ObdPidInfo pidInfo;
      if (obdPidInfo(userSig.pid, pidInfo))
      {
        copyStr(unit, sizeof(unit), pidInfo.unit);
        if (pidInfo.signal != SIG_NONE)
        {
          copyStr(name, sizeof(name), signalInfo(pidInfo.signal).name); // the gauge's own short name ("Coolant")
        }
        else
        {
          copyStr(name, sizeof(name), pidInfo.name);
        }
      }
    }

    const char *label;
    if (userSigIndex < USER_SIGS && userSigIndex % 2 == 0 && page.label[0])
    {
      label = page.label;
    }
    else if (name[0])
    {
      label = name;
    }
    else
    {
      label = signalInfo(id).name;
    }
    signalSetLabel(id, label, unit);
    lastFrameMs[userSigIndex] = 0;
  }
  ecuNowUserSigsChanged(); // the ECU receiver finds each ECU channel's place in its list again
}

bool userSigDecimals(signalId_t id, uint8_t &decimals)
{
  const UserSigCfg *userSigPointer = userSigConfig(id);
  if (userSigPointer == NULL)
  {
    return false;
  }
  const UserSigCfg &userSig = *userSigPointer; // the page value's settings, read with dots
  if (userSig.source == USRC_ME7) // 0.17.6: an ME7 variable: from its unit, else from the size of its .ecu factor (A)
  {
    const int8_t varIndex = me7FindVar(userSig.sigName);
    if (varIndex < 0)
    {
      return false; // not loaded yet
    }
    const Me7Var &variable = me7Var(varIndex);
    const char *unit = variable.unit; // already plain: "C", "deg", "% PED" ...
    float step = variable.factor;     // how much one count of the raw value is worth
    if (step < 0)
    {
      step = -step;
    }
    if (!strcmp(unit, "rpm") || !strcmp(unit, "mbar") || !strcmp(unit, "hPa") || !strcmp(unit, "C") || !strcmp(unit, "km/h"))
    {
      decimals = 0; // rpm, pressure, temperature, speed: whole numbers
    }
    else if (!strcmp(unit, "-"))
    {
      decimals = 2; // lambda and other ratios (1.00)
    }
    else if (unit[0] == '%' || !strcmp(unit, "V") || !strcmp(unit, "deg") || !strcmp(unit, "g/s") || !strcmp(unit, "ms"))
    {
      decimals = 1; // load, pedal, duty, volts, timing, air mass, injection time
    }
    else if (variable.inverse)
    {
      decimals = 2; // A / (raw - B): the step changes with the value, so two places
    }
    else if (step >= 1.0f)
    {
      decimals = 0; // whole steps
    }
    else if (step >= 0.1f)
    {
      decimals = 1;
    }
    else
    {
      decimals = 2;
    }
    return true;
  }
  if (userSig.source == USRC_CAN_DBC) // 0.36.14: a DBC signal: from its factor (0.01 = two places), as the CAN tab shows it
  {
    const uint8_t userSigIndex = id - SIG_USER_1; // this page value's place in the lookup tables
    const uint32_t generation = applyGeneration * 65599u + canDbcGeneration(); // as userSigTick works it out
    if (lookupGeneration[userSigIndex] != generation || dbcSignalIndex[userSigIndex] < 0)
    {
      return false; // not found in the DBC yet: the unit rule meanwhile
    }
    decimals = dbcDecimals[userSigIndex];
    return true;
  }
  if (userSig.source == USRC_DIAG && userSig.diagValue >= 1 && userSig.diagValue <= VDIAG_VALUES)
  {
    // 0.36.14: a Diagnostics UDS value: from its scale (0.01 = two places). KWP blocks scale themselves: the unit rule.
    const VdiagValue &vdiagValue = settings.vdiagValues[userSig.diagValue - 1];
    if (vdiagValue.kind == VDIAG_KIND_UDS)
    {
      decimals = decimalsOfScale(vdiagValue.scale, vdiagValue.offset);
      return true;
    }
  }
  if (userSig.source == USRC_ECU) // an ECU channel: as many decimals as its channel list says
  {
    EcuChannelInfo info;
    if (ecuNowChannelInfo(userSig.ecuKind, userSig.ecuChannel, info))
    {
      decimals = info.decimals;
      if (decimals > 2)
      {
        decimals = 2; // the gauge shows at most two
      }
      return true;
    }
  }
  return false;
}

bool userSigNote(signalId_t id, char *buf, size_t len)
{
  const UserSigCfg *userSig = userSigConfig(id);
  if (!userSig)
  {
    return false;
  }
  switch (userSig->source)
  {
  case USRC_KLINE:
    if (runtime.klinePage.connected && runtime.klinePage.addr == userSig->klineAddr)
    {
      snprintf(buf, len, "Block %u field %u: no value", userSig->klineGroup, userSig->klineField + 1);
    }
    else if (settings.klineProtocol == KL_ME7LOGGER)
    {
      copyStr(buf, len, "ME7 logger has the K-line"); // 0.17.6: it keeps the line (wantedOwner in can2gauge_KLine.cpp)
    }
    else
    {
      copyStr(buf, len, runtime.klinePage.state[0] ? runtime.klinePage.state : "Starting the K-line session");
    }
    return true;
  case USRC_OBD:
    if ((settings.canProfile == CAN_PROFILE_OBD2 || settings.obdOverCan) && canStats().up) // OBD-II on the wire asks on 0x7DF
    {
      snprintf(buf, len, "Waiting for PID %02X", userSig->pid);
    }
    else if (!settings.elmMac[0])
    {
      copyStr(buf, len, "No OBD adapter paired - see the web UI");
    }
    else if (elmConnected() && !strcmp(elmStatus().state, "polling"))
    {
      const ElmStatus &adapterStatus = elmStatus();
      const uint8_t supportedWord = (userSig->pid - 1) / 32;
      const bool known = adapterStatus.supported[0] != 0;
      const bool hasPid = userSig->pid && supportedWord < 3 &&
          ((adapterStatus.supported[supportedWord] >> (31 - (userSig->pid - 1) % 32)) & 1);
      if (known && !hasPid)
      {
        snprintf(buf, len, "Connected - the car has no PID %02X", userSig->pid);
      }
      else
      {
        snprintf(buf, len, "Connected - waiting for PID %02X", userSig->pid);
      }
    }
    else
    {
      snprintf(buf, len, "Bluetooth OBD: %s", elmStatus().state);
    }
    return true;
  case USRC_CAN_DBC:
  {
    CanMonEntry entry;
    bool known = false;
    canDbcLock();
    const CanDbcMessage *message = canDbcLoaded() ? canDbcFind(userSig->canId) : NULL;
    for (uint16_t signalIndex = 0; message && signalIndex < message->signalCount && !known; signalIndex++)
    {
      known = !strcmp(canDbcSignal(message->firstSignal + signalIndex).name, userSig->sigName);
    }
    canDbcUnlock();
    if (!canDbcLoaded())
    {
      copyStr(buf, len, "No DBC on the gauge - see the web UI");
    }
    else if (!known)
    {
      snprintf(buf, len, "%s isn't in %s", message ? userSig->sigName : "the message", canDbcName());
    }
    else if (!canMonitorGet(userSig->canId, entry) || millis() - entry.lastMs > 2000)
    {
      snprintf(buf, len, "0x%03lX not on the bus", (unsigned long)(userSig->canId & 0x1FFFFFFF));
    }
    else
    {
      snprintf(buf, len, "%s: no value", userSig->sigName);
    }
    return true;
  }
  case USRC_ECU:
  {
    const UserSigCfg &ecuSig = *userSig; // the page value's settings, read with dots
    ecuNowNote(ecuSig.ecuKind, ecuSig.ecuChannel, buf, len);
    return true;
  }
  case USRC_ME7: // 0.17.6: an ME7 logger variable
  {
    const UserSigCfg &me7Sig = *userSig; // the page value's settings, read with dots
    if (settings.klineProtocol != KL_ME7LOGGER)
    {
      copyStr(buf, len, "Pick ME7 logger on the K-line tab");
    }
    else if (me7State() != ME7_LOGGING)
    {
      copyStr(buf, len, runtime.klineState[0] ? runtime.klineState : "ME7 logger starting"); // "ME7 connecting" ...
    }
    else if (me7FindVar(me7Sig.sigName) < 0)
    {
      snprintf(buf, len, "%s isn't in the .ecu", me7Sig.sigName);
    }
    else
    {
      snprintf(buf, len, "%s: no value", me7Sig.sigName);
    }
    return true;
  }
  case USRC_DIAG:
  {
    const UserSigCfg &diagSig = *userSig; // the page value's settings, read with dots
    const VdiagStatus &diag = vdiagStatus();
    if (diagSig.diagValue < 1 || diagSig.diagValue > VDIAG_VALUES || settings.vdiagValues[diagSig.diagValue - 1].kind == VDIAG_KIND_NONE)
    {
      snprintf(buf, len, "Diag value %u isn't set up - Diagnostics tab", diagSig.diagValue);
    }
    else if (settings.vdiagRun == VDIAG_RUN_OFF)
    {
      copyStr(buf, len, "Diagnostics is off - see the web UI");
    }
    else if (!diag.values[diagSig.diagValue - 1].supported)
    {
      snprintf(buf, len, "The module refused it (NRC %02X)", diag.values[diagSig.diagValue - 1].lastNrc);
    }
    else if (!strcmp(diag.state, "polling"))
    {
      if (diag.values[diagSig.diagValue - 1].lastNrc == 0x22)
      {
        copyStr(buf, len, "Not available now (NRC 22)"); // e.g. a value that needs the engine running
      }
      else
      {
        copyStr(buf, len, "Connected - waiting for a value");
      }
    }
    else
    {
      snprintf(buf, len, "VAG diag: %s", diag.state);
    }
    return true;
  }
  default:
    return false;
  }
}

void userSigOnObdPid(uint8_t pid, signalId_t catalogueId, float value)
{
  (void)catalogueId;
  for (uint8_t userSigIndex = 0; userSigIndex < USER_SIGS_ALL; userSigIndex++)
  {
    if (configOf(userSigIndex).source == USRC_OBD && configOf(userSigIndex).pid == pid)
    {
      signalSet((signalId_t)(SIG_USER_1 + userSigIndex), value);
    }
  }
}

// the OBD PIDs a page shows, so the OBD polling can put them in its fast group: its gauge signals fed by a PID (rpm,
// coolant ...) and its OBD user signals
uint8_t userSigPagePids(uint8_t cfgIndex, uint8_t *pids, uint8_t maxPids)
{
  if (cfgIndex >= MAX_PAGES)
  {
    return 0;
  }
  const GaugePageCfg &page = settings.pages[cfgIndex];
  uint8_t ids[5];
  ids[0] = page.signal;  // main value
  ids[1] = page.signal2; // second value
  ids[2] = SIG_NONE;
  ids[3] = SIG_NONE;
  ids[4] = SIG_NONE;
  if (page.style == STYLE_QUAD || page.style == STYLE_TRIPLE || page.style == STYLE_CUSTOM)
  {
    ids[2] = settings.pages[cfgIndex].slots[0]; // Quad values 2-4 / the Custom centre stack
    ids[3] = settings.pages[cfgIndex].slots[1];
    ids[4] = settings.pages[cfgIndex].slots[2];
  }
  uint8_t count = 0;
  for (uint8_t i = 0; i < 5 && count < maxPids; i++)
  {
    uint8_t pid = obdPidForSignal((signalId_t)ids[i]);
    const UserSigCfg *userSig = userSigConfig((signalId_t)ids[i]);
    if (userSig && userSig->source == USRC_OBD)
    {
      pid = userSig->pid;
    }
    if (pid != 0)
    {
      pids[count] = pid;
      count++;
    }
  }
  return count;
}

// every CAN frame, in the CAN RX task (and the relay task): a page's DBC signal is decoded the moment its frame
// arrives. The signal's place in the DBC is found by userSigTick (once per DBC / settings change); until then, or while
// a DBC is being loaded, the frame is left to userSigTick.
void userSigOnCanFrame(const twai_message_t &frame)
{
  uint32_t id = frame.identifier; // the id as settings store it: bit 31 set = extended (29-bit) id
  if (frame.extd)
  {
    id |= 0x80000000UL;
  }
  // is any page reading this frame? (checked before touching the DBC lock: most frames aren't)
  bool wanted = false;
  for (uint8_t userSigIndex = 0; userSigIndex < USER_SIGS_ALL && !wanted; userSigIndex++)
  {
    wanted = configOf(userSigIndex).source == USRC_CAN_DBC && configOf(userSigIndex).canId == id;
  }
  if (!wanted || !canDbcTryLock())
  {
    return;
  }
  const uint32_t generation = applyGeneration * 65599u + canDbcGeneration(); // as userSigTick works it out
  const CanDbcMessage *message = NULL;
  if (canDbcLoaded())
  {
    message = canDbcFind(id);
  }
  uint8_t dlc = frame.data_length_code;
  if (dlc > 8)
  {
    dlc = 8;
  }
  for (uint8_t userSigIndex = 0; userSigIndex < USER_SIGS_ALL && message; userSigIndex++)
  {
    const UserSigCfg &userSig = configOf(userSigIndex);
    if (userSig.source != USRC_CAN_DBC || userSig.canId != id)
    {
      continue;
    }
    if (lookupGeneration[userSigIndex] != generation || dbcSignalIndex[userSigIndex] < 0)
    {
      continue; // not looked up yet for this DBC / these settings: userSigTick does it
    }
    float value;
    if (canDbcDecode(*message, canDbcSignal(dbcSignalIndex[userSigIndex]), frame.data, dlc, value))
    {
      signalSetBus((signalId_t)(SIG_USER_1 + userSigIndex), value);
    }
  }
  canDbcUnlock();
}

void userSigTick()
{
  bool any = false;
  for (uint8_t userSigIndex = 0; userSigIndex < USER_SIGS_ALL && !any; userSigIndex++)
  {
    any = configOf(userSigIndex).source == USRC_CAN_DBC;
  }
  if (!any)
  {
    return;
  }

  canDbcLock();
  const uint32_t generation = applyGeneration * 65599u + canDbcGeneration(); // changes with every DBC load and settings change
  for (uint8_t userSigIndex = 0; userSigIndex < USER_SIGS_ALL; userSigIndex++)
  {
    const UserSigCfg &userSig = configOf(userSigIndex);
    if (userSig.source != USRC_CAN_DBC || !canDbcLoaded())
    {
      continue;
    }
    const CanDbcMessage *message = canDbcFind(userSig.canId);
    if (!message)
    {
      continue;
    }
    if (lookupGeneration[userSigIndex] != generation) // find the signal by name once per DBC / settings change
    {
      lookupGeneration[userSigIndex] = generation;
      dbcSignalIndex[userSigIndex] = -1;
      for (uint16_t signalIndex = 0; signalIndex < message->signalCount; signalIndex++)
      {
        if (!strcmp(canDbcSignal(message->firstSignal + signalIndex).name, userSig.sigName))
        {
          dbcSignalIndex[userSigIndex] = message->firstSignal + signalIndex;
          dbcDecimals[userSigIndex] = decimalsOfScale(canDbcSignal(dbcSignalIndex[userSigIndex]).factor,
                                                      canDbcSignal(dbcSignalIndex[userSigIndex]).offset);
          char unit[10];
          normaliseDbcUnit(canDbcSignal(dbcSignalIndex[userSigIndex]).unit, unit, sizeof(unit));
          signalSetLabel((signalId_t)(SIG_USER_1 + userSigIndex), NULL, unit);
          break;
        }
      }
    }
    if (dbcSignalIndex[userSigIndex] < 0)
    {
      continue;
    }

    CanMonEntry entry;
    if (!canMonitorGet(userSig.canId, entry) || entry.lastMs == lastFrameMs[userSigIndex])
    {
      continue;
    }
    lastFrameMs[userSigIndex] = entry.lastMs;
    float value;
    if (canDbcDecode(*message, canDbcSignal(dbcSignalIndex[userSigIndex]), entry.data, entry.dlc, value))
    {
      signalSetBus((signalId_t)(SIG_USER_1 + userSigIndex), value);
    }
  }
  canDbcUnlock();
}
