package chipyard

import org.chipsalliance.cde.config.{Config}

// 用于性能模型校准：2 lane 的 Hwacha（其余与 HwachaRocketConfig 相同）
class HwachaL2RocketConfig extends Config(
  new hwacha.WithNLanes(2) ++
  new HwachaRocketConfig)

// 4 lane 的 Hwacha
class HwachaL4RocketConfig extends Config(
  new hwacha.WithNLanes(4) ++
  new HwachaRocketConfig)

// 8 lane / 16 lane 的 Hwacha
class HwachaL8RocketConfig extends Config(
  new hwacha.WithNLanes(8) ++
  new HwachaRocketConfig)
class HwachaL16RocketConfig extends Config(
  new hwacha.WithNLanes(16) ++
  new HwachaRocketConfig)

// 校准 VRU 与多 bank L2 用：关闭 VRU 的 1 lane；2 lane + 2 个 L2 bank
class WithHwachaNoVRU extends Config((site, here, up) => {
  case hwacha.HwachaBuildVRU => false
})
class HwachaNoVRURocketConfig extends Config(
  new WithHwachaNoVRU ++
  new HwachaRocketConfig)
class HwachaL2B2RocketConfig extends Config(
  new hwacha.WithNLanes(2) ++
  new freechips.rocketchip.subsystem.WithNBanks(2) ++
  new HwachaRocketConfig)
class HwachaL4B2RocketConfig extends Config(
  new hwacha.WithNLanes(4) ++
  new freechips.rocketchip.subsystem.WithNBanks(2) ++
  new HwachaRocketConfig)
class HwachaL4B4RocketConfig extends Config(
  new hwacha.WithNLanes(4) ++
  new freechips.rocketchip.subsystem.WithNBanks(4) ++
  new HwachaRocketConfig)
