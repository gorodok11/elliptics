#!/usr/bin/python
# -*- coding: utf-8 -*-

import sys
sys.path.insert(0, "bindings/python/")
from elliptics import *
import binascii
from pprint import pprint

try:
	log = Logger("/dev/stderr", 31)
	cfg = Config()
	cfg.cookie = "0123456789012345678901234567890123456789"
	cfg.config.wait_timeout = 60

	n = Node(log, cfg)
	
	n.add_remote("localhost", 1025)

	s = Session(n)

	s.add_groups([1,2,3])

	group = 1
	try:
		obj = "qwerty.xml"
		addr = s.lookup_addr(obj, group)
		print "object", obj, "should live at", addr, "in group", group
	except Exception as e:
		print "Failed to lookup in group", group, ":", e

	id = Id([1, 2, 3, 4], 1)

	# write data by ID into specified group (# 2)
	# read data from the same group (# 2)
	try:
		data = '1234567890qwertyuio'
		s.write_data(id, data, 0)
		print "WRITE:", data
		s.write_metadata(id, "", [1], 0)
		print "Write metadata"
	except Exception as e:
		print "Failed to write data by id:", e

	try:
		res = s.read_data(id, 0, 0)
		print " READ:", res
	except Exception as e:
		print "Failed to read data by id:", e

	id.type = -1
	s.remove(id)
	try:
		res = s.read_data(id, 0, 0)
		print " READ:", res
	except Exception as e:
		print "Failed to read data by id:", e

	# write data into all 3 groups specified in add_groups() call.
	# read data from the first available group
	try:
		key = "test.txt"
		data = '1234567890qwertyuio'
		s.write_data(key, data, 0, 0)
		print "WRITE:", key, ":", data
		s.write_metadata(key, 0)
		print "Write metadata"
	except Exception as e:
		print "Failed to write data by string:", e

	try:
		key = "test.txt"
		res = s.read_data("test.txt", 0, 0, 0)
		print " READ:", key, ":", res
	except Exception as e:
		print "Failed to read data by string:", e

	try:
		print s.read_latest("test.txt", 0, 0, 0);
	except Exception as e:
		print "Failed to read latest data by string:", e

	# bulk read of keys by name
	try:
		files =  s.bulk_read(["test1", "test2", "test3", "test4", "test5"], 1, 0)
		for f in files:
			print binascii.hexlify(f[:6]), ":", f[68:]
	except Exception as e:
		print "Failed to read bulk:", e

	routes = s.get_routes()
	for route in routes:
		print route[0].group_id, route[0].id, route[1]

	print "Requesting stat_log"
	pprint(s.stat_log())

except:
	print "Unexpected error:", sys.exc_info()
