#!/usr/bin/env python3

import json
import sys
import os


test_list = list();


def c_str(str):
    if str is None:
        return "NULL"

    str = str.replace("\\", "\\\\")
    str = str.replace("\n", "\\n")
    str = str.replace("\r", "\\r")
    str = str.replace("\t", "\t")
    str = str.replace("\"", "\\\"")
    return "\"" + str + "\""

def process_file(path):
    basename = os.path.basename(path)
    corename = os.path.splitext(basename)[0]

    with open(path) as json_data:
        d = json.load(json_data)
        i = 1
        for test in d["tests"]:
            if test["desc"] is not None:
                test["desc"] = test["desc"].lower()
                if test["desc"].endswith('.'):
                    test["desc"] = test["desc"][:-1]

            if test.get("data") is not None:
                data = json.dumps(test["data"])
            else:
                data = None

            if test.get("partials") is not None:
                partials = json.dumps(test["partials"])
            else:
                partials = None;

            testname = corename + "-" + str(i)
            funcname = "test_" + corename + "_" + str(i)
            test_list.append((testname, funcname))
            i += 1

            print("static void")
            print(funcname + "(void)")
            print("{")
            print("    run(")
            print("        " + c_str(test["desc"]) + ",")
            print("        " + c_str(test["template"]) + ",")
            print("        " + c_str(data) + ",")
            print("        " + c_str(partials) + ",")
            print("        " + c_str(test["expected"]))
            print("    );")
            print("}")
            print()

for path in sys.argv[1:]:
    process_file(path)

print("TEST_LIST = {");
for test in test_list:
    print("    { \"" + test[0] + "\", " + test[1] + " },")
print("    { 0 }")
print("};")
print()
