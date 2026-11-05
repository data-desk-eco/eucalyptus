# gum: one c file over libcurl (with j2k.c and nn.c), and gum.bin, the
# model in nn.c's layout, exported from the notebook's model.pth with torch
CFLAGS ?= -O3 -march=native -Wall -Wextra -Wno-unused-parameter
LDLIBS = -lcurl -lm -lpthread
R = ../notebooks/resources/s2
UV = uv run -q --python 3.10 --with torch==2.7.1 --with numpy==1.26.4 \
	--with segmentation_models_pytorch==0.4.0

all: gum gum.bin

gum: gum.c j2k.c nn.c
	$(CC) $(CFLAGS) -o $@ gum.c $(LDLIBS)

gum.bin: export.py $(R)/model.pth $(R)/radtran_lookup_table.csv
	$(UV) python export.py $(R)/model.pth $(R)/radtran_lookup_table.csv $@

# the notebook's three scenes, from google's mirror (baseline 05.00), and
# each target against the notebook's own steps
check: gum gum.bin
	./gum gum.bin < test.tsv > test.jsonl
	cd .. && $(UV) --with pandas==2.2.3 --with scikit-image==0.25.1 \
	  --with rasterio==1.3.10 --with xarray==2025.6.0 --with shapely==2.0.4 \
	  --with matplotlib==3.10.3 --with pyproj==3.6.1 --with requests \
	  --with python-dateutil python gum/check.py gum/gum gum/gum.bin \
	  gum/test.tsv gum/test.jsonl

clean:
	rm -f gum gum.bin test.jsonl

.PHONY: all check clean
