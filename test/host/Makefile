# Runs every host test (no ESP-IDF needed): make -C test/host
SUITES := proto_roundtrip link dot11_parser aggregator channel_planner ota pairing ble nmea raw check_passive

.PHONY: test clean $(SUITES)

test: $(SUITES)

$(SUITES):
	$(MAKE) -C $@ test

clean:
	rm -rf build
