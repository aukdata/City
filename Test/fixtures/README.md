# Road format fixture

`road_v18.bin` is a retained synthetic output from `Construction.StageClockAndResume`, generated before the v19 coordinate change. It contains two nodes, one road and one construction plan. It has no user city data.

Expected node 0: (432, 0, 512). Edge control points: (480, 0, 506), (544, 0, 518). Stored construction start: 1000; stored duration: 100. The snapshot-mode reader must preserve that duration; the legacy reader retains its original recalculation behavior.
