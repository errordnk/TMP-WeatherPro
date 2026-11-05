# Icons

The weather icons for the OpenWeather API (`WeatherPro/OwSvgIcons.h`) are line icons from [Lucide](https://lucide.dev)
(ISC license). They have the same style (24x24 viewBox, stroke width 2, rounded) as the CPU/GPU/RAM icons of the
TrafficMonitor taskbar window.

They are monochrome (`stroke="currentColor"`): the plugin rasterizes them with Direct2D in the size it needs, as high
as the text of the taskbar window, and draws them black on a light taskbar and white on a dark one. 13 icons are used,
in the order of `WapiOpenWeather::GetWeatherIconIndex`:

| Index | OpenWeather codes | Icon |
| --- | --- | --- |
| 0 | `01d` | `sun` |
| 1 | `01n` | `moon` |
| 2 | `02d` | `cloud-sun` |
| 3 | `02n` | `cloud-moon` |
| 4 | `03d`, `03n` | `cloud` |
| 5 | `04d`, `04n` | `cloudy` |
| 6 | `09d`, `09n` | `cloud-rain` |
| 7 | `10d` | `cloud-sun-rain` |
| 8 | `10n` | `cloud-moon-rain` |
| 9 | `11d`, `11n` | `cloud-lightning` |
| 10 | `13d`, `13n` | `cloud-snow` |
| 11 | `50d`, `50n` | `cloud-fog` |
| 12 | any other code | `circle-question-mark` |

SVG rendering needs Windows 10 1703 or later (`ID2D1DeviceContext5`). The QWeather and weather.com.cn icons are still PNG sprites.