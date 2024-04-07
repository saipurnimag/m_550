import json

f1 = json.load(open("/Users/purnimag/Documents/CSCI550/mongo/data/dataset_updated.json"))
f2 = json.load(open("/Users/purnimag/Documents/CSCI550/mongo/data/No-SqlDataset.json"))

for i in f2:
    f1.append(i)


#save the combined json file
json.dump( f1,open("/Users/purnimag/Documents/CSCI550/mongo/data/combined.json", "w"), indent=-1)
