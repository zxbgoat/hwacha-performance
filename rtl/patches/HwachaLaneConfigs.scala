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
