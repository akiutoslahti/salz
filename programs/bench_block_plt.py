#!/usr/bin/env python3
# encoding: utf-8

"""bench_block_plt.py - Plot csv formatted output generated with
bench_block tool."""

# Copyright (c) 2021 Aki Utoslahti. All rights reserved.
#                                                                     
#  This work is distributed under terms of the MIT license.
#  See file LICENSE or a copy at <https://opensource.org/licenses/MIT>.

import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator

def main():
    if len(sys.argv) < 2:
        print(f"Invalid arguments\n")
        print(f"Usage: {sys.argv[0]} [file1] [file2] ...")
        return

    for argstr in sys.argv[1: ]:
        infile = argstr
        outfile = os.path.splitext(infile)[0] + ".png"

        try:
            data = pd.read_csv(infile)
        except:
            print(f"Could not read {infile}")
            return

        fig_data = data.copy()
        fig_data = data.set_index("block size (log2)")
        fig_data = fig_data.drop(["block size (b)"], axis = 1)

        fig_count = len(fig_data.columns)
        cols = 2
        rows = fig_count // cols + fig_count % cols

        fig, ax = plt.subplots(rows, 2, figsize = (10 * cols, 5 * rows))
        fig.suptitle(infile)
        for i in range(fig_count):
            row = i // cols
            col = i % cols
            axdata = fig_data.iloc[:, i]
            ax[row, col].plot(axdata, linewidth = 2)
            ax[row, col].set_title(
                    f"{fig_data.columns[i]}, for {fig_data.index.name}")
            ax[row, col].plot(axdata.idxmin(), axdata.min(), marker = "o",
                              markersize = 10, color = "red")
            ax[row, col].xaxis.set_major_locator(MaxNLocator(integer=True))
        if fig_count % cols != 0:
            ax[-1, -1].axis("off")
        
        try:
            plt.savefig(outfile)
        except:
            print(f"Could not write to {outfile}")

        print(f"Filename: {infile}")
        print(data, end="\n\n")

if __name__ == "__main__":
    main()
