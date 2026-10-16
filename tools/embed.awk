# SPDX-License-Identifier: GPL-2.0-or-later
# Turn a text file into `const char name[] = "...";`
BEGIN { printf "/* generated from %s, do not edit */\nconst char %s[] =\n", ARGV[1], name }
{
	gsub(/\\/, "\\\\")
	gsub(/"/, "\\\"")
	gsub(/\t/, "\\t")
	printf "\t\"%s\\n\"\n", $0
}
END { print "\t;" }
