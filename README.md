# nRF5340 Wearable — 공기질 측정 노드

nRF5340 DK (Zephyr / nRF Connect SDK) 에 미세먼지·VOC·온습도 센서 3개를 물려
1초마다 측정값을 읽고 시리얼로 출력하는 펌웨어.

## 데이터를 어떻게 받아오나

**전부 I2C 하나로 받는다.** 센서 3개가 `i2c1` 버스를 공유하고 주소로만 구분된다.

| 센서 | I2C 주소 | 측정 항목 | 접근 방식 |
|---|---|---|---|
| Sensirion **SEN54** | `0x69` | PM1/2.5/4/10, 온도, 습도, VOC 지수 | Sensirion 공식 C 라이브러리 + 직접 만든 Zephyr HAL |
| Bosch **BMV080** | `0x54` | PM1/2.5/10 | Bosch 프리빌트 바이너리 라이브러리 + 콜백 2개 구현 |
| Sensirion **SHT40** | `0x44` | 온도, 습도 | Zephyr 메인라인 `sht4x` 드라이버 (sensor API) |

배선: `i2c1` = **P1.02 (SDA) / P1.03 (SCL)**, 100 kHz (SEN5x 최대 속도).
핀 배정과 센서 노드는 `boards/nrf5340dk_nrf5340_cpuapp.overlay` 에 정의돼 있고,
코드는 `I2C_DT_SPEC_GET(DT_NODELABEL(...))` 로 버스+주소를 꺼내 쓴다.

풀업은 nRF 내부 풀업(`bias-pull-up`, 약 13 kΩ)을 켜뒀다. 벤치 테스트용이고,
실사용에서는 외부 10 kΩ 를 다는 게 맞다.

### 센서별로 경로가 다른 이유

세 센서 모두 물리적으로는 같은 I2C 버스지만, 위에 얹힌 소프트웨어 계층이 다르다.

**SEN54** — Sensirion 공식 라이브러리(`src/sen5x/`)를 그대로 가져오고,
그게 요구하는 5개 HAL 함수만 Zephyr I2C로 매핑했다 (`sensirion_i2c_hal.c`).
사실상 `i2c_read()` / `i2c_write()` 두 줄이 전부다.

```c
sen5x_start_measurement();
sen5x_read_measured_values(&pm1p0, &pm2p5, &pm4p0, &pm10p0, &rh, &temp, &voc, &nox);
```

값은 정수 스케일로 온다 — PM `/10` µg/m³, 습도 `/100` %, 온도 `/200` °C, VOC `/10`.

**BMV080** — Bosch가 소스를 안 준다. `lib_bmv080.a` + `lib_postProcessor.a`
프리빌트 아카이브에 읽기/쓰기/딜레이 함수 포인터를 넘겨주는 구조라, `main.c` 에
그 3개만 구현했다. 프로토콜은 16비트 헤더 + 빅엔디안 16비트 워드 N개이고,
헤더의 MSB가 R/W 비트라 Zephyr가 이미 주소에 접어넣은 만큼 헤더를 왼쪽으로 1비트
밀어서 2바이트 레지스터 주소처럼 보낸다.

측정값은 폴링해서 콜백으로 받는다. **최소 1초에 한 번은 서비스해줘야 한다.**

```c
bmv080_serve_interrupt(bmv, bmv080_data_ready, NULL);  /* 새 데이터 있으면 콜백 호출 */
```

**SHT40** — Zephyr 메인라인에 드라이버가 이미 있어서 아무것도 안 짰다.
오버레이에 `compatible = "sensirion,sht4x"` 만 적고 표준 sensor API로 읽는다.

```c
sensor_sample_fetch(sht40);
sensor_channel_get(sht40, SENSOR_CHAN_AMBIENT_TEMP, &t);
```

## 동작

`main()` 은 1초 주기 루프다. SEN54는 매번 폴링, BMV080은 `serve_interrupt()` 로
새 데이터가 준비됐을 때만 콜백이 뜨고, SHT40은 매번 fetch 한다. 출력은 `printk()` —
RTT/UART 시리얼 터미널로 보면 된다.

부팅 시 SEN54는 응답할 때까지 1초 간격으로 계속 재시도한다. 배선을 고치는 동안
재플래시할 필요가 없게 하려는 것. BMV080/SHT40은 없으면 경고만 찍고 건너뛰므로,
센서 하나만 꽂아도 돌아간다.

미세먼지는 한국 AQI 기준으로 등급(`GOOD` / `MODERATE` / `BAD` / `VERY BAD`)을 같이
찍고, PM10 ≥ 150 또는 PM2.5 ≥ 75 가 **2시간 연속** 지속되면 경고를 띄운다.

## 빌드

```bash
west build -b nrf5340dk/nrf5340/cpuapp
west flash
```

`prj.conf` 에서 주의할 것 두 가지:

- `CONFIG_FPU=y` — Bosch 라이브러리가 m33f(하드플로트)로 빌드돼 있어서 float ABI가
  안 맞으면 링크가 깨진다.
- `CONFIG_MAIN_STACK_SIZE=16384` — Bosch 파티클 알고리즘이 스택을 많이 쓴다.
  기본 8K에서는 오버플로우 났다.

## 출력 예시

```
--------------- SEN54 MEASUREMENT ------
  PM1.0 : 5.2 ug/m3
  PM2.5 : 6.8 ug/m3
  PM4.0 : 7.1 ug/m3
  PM10  : 7.3 ug/m3
  Temp  : 24.31 C
  Humid : 47.52 %
  VOC   : 98.4

--------------- BMV080 MEASUREMENT -----
  PM1.0 : 4.87 ug/m3
  PM2.5 : 6.12 ug/m3  [GOOD]
  PM10  : 6.90 ug/m3  [GOOD]
```

## 관련 저장소

재실 감지(구역별 근로자 수 / 체류 시간)는 별도 저장소:
[JeonJunYoung-hub/IWRL6432](https://github.com/JeonJunYoung-hub/IWRL6432) — TI mmWave
레이더, I2C가 아니라 **UART** 로 통신한다.

## 라이선스 주의

`src/bmv080/*.a` 는 Bosch 프리빌트 바이너리다. 재배포 조건은 Bosch 라이선스를 확인할 것.
