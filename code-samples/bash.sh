#!/bin/sh
# SCRIPT:  armstrongnumber.sh
# USAGE:   armstrongnumber.sh
# PURPOSE: Check if the given number is Armstrong number ?
#                         \\\\ ////
#                         \\ - - //
#                            @ @
#                    ---oOOo-( )-oOOo---
# Armstrong numbers are the sum of their own digits to the power of
# the number of digits. As that is a slightly brief wording, let me
# give an example:
# 153 = 1³ + 5³ + 3³
# Each digit is raised to the power three because 153 has three
# digits. They are totalled and we get the original number again!
#Notice that Armstrong numbers are base dependent,but we'll mainly be
# dealing with base 10 examples.The Armstrong numbers up to 5 digits
# are 1 to 9,153, 370, 371, 407, 1634, 8208, 9474, 54748, 92727,93084
#
#####################################################################
#                     Script Starts Here                            #
#####################################################################

echo -n "Enter the number: "
read Number
Length=${#Number}
Sum=0
OldNumber=$Number

while [ $Number -ne  0 ]
do
     Rem=$((Number%10))
     Number=$((Number/10))
     Power=$(echo "$Rem ^ $Length" | bc )
     Sum=$((Sum+$Power))
done

if [ $Sum -eq $OldNumber ]
then
    echo "$OldNumber is an Armstrong number"
else
    echo "$OldNumber is not an Armstrong number"
fi