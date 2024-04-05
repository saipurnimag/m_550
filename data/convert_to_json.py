import json

file = open("dataset_updated.txt", "r")
cnt = 0
data_json =[]
for line in file:
    a,b = line.split("::::")
    data_json.append({"text":a,"label":int(b)})
    
file2 = open("dataset_updated.json", "w")
json.dump(data_json, file2, indent=0)
file2.close()
