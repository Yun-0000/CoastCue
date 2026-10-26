# CoastCue validation and early impact brief

## 1. User validation

I interviewed a few vessel operators at the Xingjian Road wharf. Between them they cover small cargo and transport boats and nearshore fishing. Before showing them CoastCue, I asked about their vessels, how far from shore they usually go, what they use to communicate, and what they do when communication fails at sea.

For routine nearshore trips they rely on mobile phones, which need cell coverage. When communication fails at sea, there is no backup channel, and repairs have to wait until the boat is back in port. One operator described it this way:

> Me: Have you ever lost communication out there?
>
> Operator: Yes.
>
> Me: So how do you usually deal with it?
>
> Operator: You have to come back and get it fixed.
>
> *Translated from Chinese.*

Larger, regulated commercial vessels already carry the satellite equipment they are required to have, including state-subsidized Beidou terminals. CoastCue fits the smaller recreational and nearshore boats that have no satellite gear, as a second channel that does not depend on cell coverage.

## 2. Field validation

### Zhoushan, at sea

On 13 August 2026 we tested the CN 433 MHz build at the Xingjian Road wharf of the Zhoushan International Aquatic City, on a sunny day with a calm sea. One person ran the boat unit, one watched the shore unit and one filmed. Distances were estimated from a map.

| Distance | Scenario | Result |
| --- | --- | --- |
| Near shore | Help and SOS | Both completed the full alert → acknowledge → resolve cycle. |
| ~1 km | Help | Completed the full cycle. |
| ~2.5 km | Help, then SOS | SOS replaced the Help, and the full cycle completed. |

### Shoreline Lake, Mountain View

On 21 October 2026 friends took the boat unit out on Shoreline Lake in Mountain View, California. This round used the US 915 MHz build, which sends each message in under half a second instead of several seconds and is more stable than the Zhoushan setup, with the boat unit now in an acrylic case and a waterproof bag. The shore unit and the iPhone stayed on the lakeshore, and all three screens were filmed.

| Round | Result |
| --- | --- |
| SOS | The shore unit showed it 0.3 s after the boat sent it, and the iPhone 0.4 s after. It was acknowledged and resolved in the app, and the boat showed both. |
| Help | The iPhone showed it about 1 s after the boat sent it. |

## 3. Early impact

Best results so far:

- Full alert-to-resolution cycles at up to about 2.5 km at sea (Zhoushan).
- Alerts reached the shore and the phone in under two seconds (Shoreline Lake).
- We piloted CoastCue with eight Zhoushan fishermen.

CoastCue already works end to end. The boat sends Help or SOS, the shore sounds its alarm, and when the alert is acknowledged and resolved on shore, the boat sees both. Both field tests ran this full loop without a cell network or internet connection. If no phone is at hand, the shore's own screen and buzzer still carry the alert.
