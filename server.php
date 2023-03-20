<?php
  
    # access control
    #header("Access-Control-Allow-Origin: https://chatsociety.org");
    header("Access-Control-Allow-Origin: *");

    # check has valid data
    if(!isset($_GET['a']) && !isset($_GET['b']))
    {
        //echo "no data provided";
        header("HTTP/1.1 200 OK");
        exit;
    }

    # get user ip hash salted by user client id
    $id = hash('sha1', $_SERVER['REMOTE_ADDR'] . $_GET['b']);
    
    # store the user data if valid lengths
    if(array_sum(count_chars($_GET['a'])) != 16 || array_sum(count_chars($_GET['b'])) != 4)
    {
        //echo "wrong size";
        header("HTTP/1.1 200 OK");
        exit;
    }

    # store user data
    file_put_contents("/dev/shm/" . $id, $_GET['a'] . $_GET['b'], LOCK_EX);

    # remove any timed out users while spitting out an array of users
    $files = glob("/dev/shm/*");
    foreach($files as $fn)
    {
        if(time()-filemtime($fn) > 3)
        {
            unlink($fn);
            continue;
        }
        echo file_get_contents($fn);
    }

?>