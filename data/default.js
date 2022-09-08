$(window).on('obsMediaPlayPause', function (e) {
    if ($('video').length == 0) return;
    if ($('video')[0].paused) $('video')[0].play();
    else $('video')[0].pause();
});

$(document).ready(function () {
    if ($("img").length > 0) {
        zoomImage(false);
    }

    $("img").on("click", function (e) {
        // An issue here which is not solved buy "painted" over is
        // the fact that the cef browser automatically zooms the image on click.
        // I did not find a way to prevent this behaviour. This was the closest I found on the issue:
        // https://stackoverflow.com/questions/65014723/how-can-i-prevent-images-from-being-zoomed-in-out-when-clicked
        // But only works in fully fledged browser not cef.

        toggleZoom();
    });

    $("img").on('wheel', { passive: false }, function (e) {
        if (e.ctrlKey) {
            e.preventDefault();

            var zoom_percent = 10 / 100 * $(window).width();
            var img_width = parseInt($("img").css("width"));
            var img_height = parseInt($("img").css("height"));
            var direction = e.originalEvent.wheelDelta > 0 ? 1 : -1;
            var new_width = clamp(img_width + direction * zoom_percent, 500, 4000);
            var new_height = new_width * img_height / img_width;

            $("img").css("width", new_width + "px");
            $("img").css("height", new_height + "px");
        }
    });
});

function zoomImage(fullscreen) {
    var fullscreen_margin = 34;
    if ($("img").length == 0) return;

    var img = $("img")[0];
    var img_height = $(img).height();
    var img_width = $(img).width();
    var ratio = img_height / img_width;
    var new_width = 0;
    var new_height = 0;

    // If image is aproximately already fullscreen then ignore
    if (fullscreen && img_width <= window.innerWidth && img_width >= window.innerWidth - fullscreen_margin &&
        img_height <= window.innerHeight && img_height >= window.innerHeight - fullscreen_margin) return;

    $(img).css("width", "");
    $(img).css("height", "");

    if (!fullscreen) {
        if (img_width > img_height) {
            new_width = window.innerWidth;
            new_height = ratio * new_width;
        } else {
            new_height = window.innerHeight;
            new_width = new_height * 1 / ratio;
        }
    } else {
        if (img_width > img_height) {
            new_height = window.innerHeight - fullscreen_margin;
            new_width = new_height * 1 / ratio;
        } else {
            new_width = window.innerWidth - fullscreen_margin;
            new_height = ratio * new_width;
        }
    }

    $(img).css("height", new_height + "px");
    $(img).css("width", new_width + "px");
}

var image_state = 0;
function toggleZoom() {
    var fullscreen;

    image_state = 1 - image_state;
    fullscreen = (image_state == 1);
    zoomImage(fullscreen);
}


function blog(str) {
    console.log("LOG_INFO: " + str);
}

const clamp = (num, min, max) => Math.min(Math.max(num, min), max);