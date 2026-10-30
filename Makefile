# Bileşenler bağımlılık sırasıyla derlenir.
ALT := src/cikti src/eis src/giris src/olay src/clipboard src/capture src/netmon src/portal src/oturum

.PHONY: all clean $(ALT)

all: $(ALT)

src/eis: src/cikti
src/giris: src/eis src/cikti
src/olay: src/giris src/eis src/cikti
src/capture: src/cikti
src/portal: src/olay src/clipboard src/capture src/netmon
$(ALT):
	$(MAKE) -C $@

clean:
	for d in $(ALT); do $(MAKE) -C $$d clean || exit 1; done
