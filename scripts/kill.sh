FRAMEWORK=doca
SIZE=big
if [ "$1" = "a" ]
then
    FRAMEWORK=astraea
fi
if [ "$2" = "s" ]
then
    SIZE=small
fi

pkill -USR1 -f "./build/src/example/$FRAMEWORK/ec_create_$FRAMEWORK -j config/ec_create_$SIZE.jsonc"