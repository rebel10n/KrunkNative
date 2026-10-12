#!/usr/bin/env python3
"""Prints a map whose objects set every ObjectInstance key to non-default and edge values (numbers,
numeric strings, 0, "", null, booleans, legacy v3.1.1 keys) on every prefab: the fuzz input of the
editor_serialize_test golden. Regenerate the golden with

    python3 -I tools/editor/make_edge_objects.py > ~/.cache/edge_objects.json
    node tools/editor/dump_oi_golden.js --defaults shared/tests/maps/v7_all_prefabs.json \
        shared/tests/maps/v7_legacy_v3.json ~/.cache/edge_objects.json > client/tests/data/editor_v7_golden.json
"""
import json
import random
import sys


NUM = ["l", "pl", "ns", "nai", "wj", "gp", "ip", "pe", "v", "b", "bm", "cr", "en", "h", "cdy", "sd", "rsc", "pr", "pc", "ps",
       "psp", "pg", "pd", "psc", "pb", "tm", "da", "o", "d", "fd", "ts", "td", "fct", "fs", "sp", "su", "sup", "tac", "tam",
       "w", "wr", "rw", "rwg", "wpi", "ntx", "bb", "bo", "smm", "ant", "ch", "n", "m", "tcp", "ab", "nf", "nm", "md", "cl",
       "stc", "nid", "ct", "te", "tce", "sfd", "cha", "sot", "bv", "sf", "nsf", "sa", "ldt", "ldo", "ldr", "rt", "rr", "in",
       "tin", "wd", "dp", "obi", "obl", "camt", "frc", "wh", "ws", "tx", "tz", "fc", "thl", "ths", "es", "esa", "ev", "er",
       "ed", "edm", "ero", "ef", "ms", "spn", "spa", "cst", "rcv", "fcr", "as", "cp", "to", "ssp", "txs", "skin", "li", "lds",
       "ld", "opn", "rtp", "rbt", "sds", "rmt", "rmr", "aic", "tsd", "spd", "prs", "prg", "prgl", "prgt", "prsc", "prms",
       "pjs", "pjd", "vd", "idl", "dth", "atk", "atkh", "mel", "melh", "agr", "fr", "aac", "shb", "sha", "shof", "shdy",
       "mldy", "mls", "ss", "crd", "hr", "mlr", "mld", "grv", "rewm", "rewr", "rewn", "bh", "pht", "xv", "tAi", "tPl", "aif",
       "cs", "igos", "dos", "ivos", "cnh", "ail", "ains", "myo", "mro", "exp", "se", "he", "bbS", "bbD", "btc", "btl", "btt",
       "bthm", "btdm", "btrm", "btsm", "btfc", "bttc", "bttt", "btfcr", "btat", "btft", "btwt", "btmt", "btsh", "btcr", "btsd",
       "btrd", "btny", "btno", "btjm", "btlm", "btvc", "btgm", "btr", "btrs", "btrc", "btcpr", "btcd", "btcf", "btcht", "btch",
       "btcbk", "btcb", "btcw", "btcwr", "btcs", "btcp", "btcm", "hbh", "hbw", "ft", "rs", "alt", "ec", "lp", "la", "sty",
       "krs", "sci", "ten", "cstc", "tv", "msp", "sat", "cpx", "zp", "tox", "toy", "tro", "tsr", "tsm", "spt", "t",
       "ambInd", "ambIndC", "skyDomeMovD", "skyDomeMovT", "shadowR", "shadowD", "ambientI", "lightD", "lightI", "sunAngX",
       "sunAngY", "fogD", "toneMapping", "toneMappingExposure", "outputEncoding"]
STR = {"st": "Hello, World! <b>", "ldc": "Kills (x)", "inm": "Press F", "ain": "Zombie", "btn": "Bot_1", "nt": "node_a",
       "fp": "1,2,3", "phi": "4,5", "rew": "1,-2", "pra": "123g", "ast": "Idle", "aid": "1234q", "at": "77v",
       "skyDomeEmisTex": "55t", "skyDomeTexA": "56x", "sc": "#ff0000", "so": "#00ff00", "sb": "#0000ff", "c": "#abcdef",
       "e": "#123456", "skyDomeCol0": "#111111", "skyDomeCol1": "#222", "skyDomeCol2": "nope", "skyDomeEmis": "#333333",
       "ambientC": "#444444", "light": "#555555", "sky": "#666666", "fogC": "#777777", "f": "2a"}
EDGE = [0, 1, 2, 0.37, 1.005, -3, "7", "", None, True, "x", 12.345, 45, 360, 1000]


def tas(rnd):
    out = []
    for _ in range(rnd.randint(0, 7)):
        a = {"ta": rnd.choice([0, 3, 14, 34, 54, 71, "5"]), "tc": rnd.choice([0, 5, -2, 1.5]), "tc2": rnd.choice([0, 9]),
             "tce": rnd.choice([0, 1, 2]), "tct": rnd.choice(["", "abc", "bad<chars>", 5]), "tcc": rnd.choice(["", "key_1", "k!"]),
             "tad": rnd.choice([0, 0.25, 3]), "cd": rnd.choice([0, 1, 4, "2"]), "cv0": rnd.choice([None, 0, "a", 3]),
             "es": rnd.choice([0, 4]), "esa": rnd.choice([0, 99]), "ev": rnd.choice([1, 0.555, 2]), "er": rnd.choice([1, 0.66]),
             "ef": rnd.choice([0, 2])}
        out.append(rnd.choice([a, {}, None, 0]))
    return out


def main():
    rnd = random.Random(714)
    objects = []
    for i in range(97):
        for variant in range(4):
            o = {"i": i, "p": [rnd.choice([1.005, -2.345, 10]), 0.004, "3"], "s": [rnd.choice([5, 0.004, 12.345]), 7, "8"]}
            if variant == 0:  # typical non-default values
                for k in NUM:
                    o[k] = rnd.choice([1, 2, 0.37, 5, 12.345])
                o.update(STR)
                o["t"] = rnd.choice([1, 2, 11, 17])
                o["r"] = [0.1, 0.004, 0]
                o["lt"] = [1, 2.345, 3]
                o["tas"] = tas(rnd)
            elif variant == 1:  # edge values
                for k in NUM:
                    o[k] = rnd.choice(EDGE)
                o["t"] = rnd.choice([0, 1, 2, 5, 19, None, "", "brick", "WALL"])  # (v7 throws on an invalid texture)
                for k in STR:
                    o[k] = rnd.choice(["", 0, 5, None, "#FFFFFF", "#000000", "zz", 1234])
                o["r"] = rnd.choice([[0, 0, 0], [0.001, 0, 0], [1], []])
                o["f"] = rnd.choice(["3f", "0", "zz", "7f", [1, 0, 1, 1, 1, 1], ""])
            elif variant == 2:  # legacy v3.1.1 keys and the single-action trigger
                o.update({"col": 1, "hp": 50, "wid": 3, "tr": 1.2, "ta": rnd.choice([14, 3]), "tc": 7, "tct": "msg", "tce": 2,
                          "es": 3, "ev": 0.5, "er": 2, "ef": 1, "cd": 1, "cv0": "k", "f": [1, 1, 0, 1, 1, 1], "dyn": 1, "gy": 2})
            else:  # nothing but the prefab, plus keys v7 does not know
                o = {"i": i, "p": [0, 0, 0], "zzz": [1, 2], "sad": 1}
            objects.append(o)
    json.dump({"name": "edge", "objects": objects, "spawns": []}, sys.stdout, separators=(",", ":"))
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
