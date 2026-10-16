# Changelog

## 0.2.0

- **Per-slot arming.** Arm state is now kept per slot instead of once for the whole device. A new `ClaimSlots`
  command (id `0x02`: counter, then a 32-bit mask of slots) disarms only the named slots; slots a claim leaves out
  arm on their own 45 s after their last claim. The original `Claim` (id `0x00`, one byte) is unchanged and still
  claims every slot, so existing controllers keep working. Verified on hardware: a claim naming every slot but one
  leaves exactly that slot armed.
- A claim that takes over a slot with a dim running stops that slot's dim, and only that slot's.
- `armed` reports whether any slot in use is armed, and the periodic status line shows the armed slots as a mask.
- The log says which slots are armed when only some are (`ARMED for some Picos: slots 0x... are not being claimed`)
  and warns about any rejected command.
- Zigbee2MQTT converter: new `claimSlots`; the cluster definition is registered again before each use, because
  Zigbee2MQTT does not re-register a device's custom cluster when a converter file is reloaded and would answer a
  newly added command with `UNSUP_COMMAND` until it restarted.
- Moved out of the combined repository: the Home Assistant integration is now
  [lutbridge-ha](https://github.com/davemhammer/lutbridge-ha).

## 0.1.0

First working firmware: receive-only CC1101 and decoder, Zigbee router with a 24-slot table, On/Off and Level
Control from bound endpoints, `Claim` and `SetSlot` commands, Zigbee2MQTT converter.
