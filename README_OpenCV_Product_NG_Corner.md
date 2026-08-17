# Hướng dẫn kiểm tra góc bo NG của sản phẩm bằng C++ OpenCV

## 1. Mục tiêu

Tài liệu này hướng dẫn xây dựng một thuật toán **OpenCV thuần C++**,
không dùng YOLO/Deep Learning, để phân biệt sản phẩm **OK / NG** khi:

-   Sản phẩm chạy trên băng chuyền.
-   Sản phẩm có thể nghiêng nhiều góc khác nhau.
-   Không thể giả định sản phẩm luôn nằm ngang hoặc dọc theo ảnh camera.
-   Trường hợp khó nhất có thể nghiêng gần góc lớn nhất như ảnh mẫu đã
    gửi.
-   Lỗi NG nằm ở **hai góc phía BOTTOM của sản phẩm bị bo cong**, trong
    khi sản phẩm OK có biên dạng góc khác.

Ý tưởng cốt lõi:

> Không xác định "góc trái dưới / phải dưới" bằng tọa độ X/Y của camera.
> Trước tiên phải đưa sản phẩm về **hệ tọa độ riêng của sản phẩm**, sau
> đó mới kiểm tra hai góc BOTTOM.

------------------------------------------------------------------------

# 2. Không cần YOLO

Với bài toán hiện tại, chưa cần YOLO.

Nếu:

-   nền tương đối ổn định,
-   sản phẩm có thể tách được khỏi nền,
-   kích thước sản phẩm không biến đổi quá lớn,

thì OpenCV có thể làm theo pipeline:

``` text
Ảnh camera
    |
    v
ROI
    |
    v
Threshold / Edge
    |
    v
Morphology
    |
    v
Find Contour
    |
    v
Lọc contour sản phẩm
    |
    v
Tìm orientation
    |
    v
Xác định TOP / BOTTOM
    |
    v
Tạo hệ tọa độ PRODUCT
    |
    v
Chuẩn hóa contour
    |
    v
Kiểm tra Bottom-Left / Bottom-Right
    |
    v
OK / NG
```

Điểm quan trọng nhất là **orientation + xác định BOTTOM**.

------------------------------------------------------------------------

# 3. Tại sao không được dùng Y lớn nhất để tìm góc dưới?

Ví dụ camera nhìn sản phẩm thẳng:

``` text
Camera coordinate

Y
|
|     TL-------------TR
|     |               |
|     |    PRODUCT    |
|     |               |
|     BL-------------BR
|
+-------------------------- X
```

Khi sản phẩm nghiêng:

``` text
                 TR
                /
               /
          PRODUCT
             /
            /
          BL
```

Lúc này `Y` của camera không còn tương ứng với TOP/BOTTOM của sản phẩm.

Nếu code kiểu:

``` cpp
if (point.y > ...)
{
    // bottom
}
```

thì ở góc nghiêng lớn có thể lấy nhầm vùng biên.

Vì vậy:

``` text
KHÔNG:
Camera X/Y
    |
    +--> tìm góc dưới

NÊN:
Camera X/Y
    |
    +--> tìm orientation
             |
             +--> tạo Product X/Y
                         |
                         +--> tìm Bottom
```

------------------------------------------------------------------------

# 4. Ý tưởng hệ tọa độ PRODUCT

Ta định nghĩa:

``` text
                 TOP
                  ^
                  |
                  | local Y
                  |
       LEFT <-----+-----> RIGHT
                  |
                  |
                  v
                BOTTOM
```

Trong hệ tọa độ này:

-   `local X < 0` = bên trái sản phẩm.
-   `local X > 0` = bên phải sản phẩm.
-   `local Y < 0` = phía TOP.
-   `local Y > 0` = phía BOTTOM.

Khi sản phẩm quay/nghiêng trong camera:

``` text
Camera:

       PRODUCT
          /
         /
        /
```

sau khi transform sang hệ tọa độ PRODUCT:

``` text
PRODUCT:

       TOP
        ^
        |
   +---------+
   |         |
   |         |
   |         |
   +---------+
        |
        v
     BOTTOM
```

Do đó thuật toán chỉ cần viết **một lần**.

------------------------------------------------------------------------

# 5. Bước 1 - Tách sản phẩm khỏi nền

Nếu nền tương đối ổn định, có thể bắt đầu bằng threshold.

Ví dụ:

``` cpp
cv::Mat gray;
cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

cv::Mat binary;

cv::threshold(
    gray,
    binary,
    thresholdValue,
    255,
    cv::THRESH_BINARY
);
```

Nếu sản phẩm tối trên nền sáng:

``` cpp
cv::threshold(
    gray,
    binary,
    thresholdValue,
    255,
    cv::THRESH_BINARY_INV
);
```

Sau đó morphology:

``` cpp
cv::Mat kernel = cv::getStructuringElement(
    cv::MORPH_ELLIPSE,
    cv::Size(5, 5)
);

cv::morphologyEx(
    binary,
    binary,
    cv::MORPH_CLOSE,
    kernel
);

cv::morphologyEx(
    binary,
    binary,
    cv::MORPH_OPEN,
    kernel
);
```

Kích thước kernel phải thử theo ảnh thực tế.

Không nên dùng kernel quá lớn vì có thể làm mất chính phần bo góc NG.

------------------------------------------------------------------------

# 6. Bước 2 - Tìm contour

``` cpp
std::vector<std::vector<cv::Point>> contours;

cv::findContours(
    binary,
    contours,
    cv::RETR_EXTERNAL,
    cv::CHAIN_APPROX_NONE
);
```

Lọc contour theo diện tích:

``` cpp
double minArea = 1000.0;

std::vector<cv::Point> productContour;

for (const auto& contour : contours)
{
    double area = cv::contourArea(contour);

    if (area > minArea)
    {
        if (productContour.empty() ||
            area > cv::contourArea(productContour))
        {
            productContour = contour;
        }
    }
}
```

Trong dây chuyền thực tế nên thêm:

-   giới hạn ROI,
-   diện tích min/max,
-   width/height,
-   aspect ratio.

------------------------------------------------------------------------

# 7. Bước 3 - Tìm orientation

Có thể thử PCA trước.

``` cpp
cv::Mat dataPoints((int)productContour.size(), 2, CV_64F);

for (size_t i = 0; i < productContour.size(); ++i)
{
    dataPoints.at<double>((int)i, 0) =
        productContour[i].x;

    dataPoints.at<double>((int)i, 1) =
        productContour[i].y;
}

cv::PCA pca(
    dataPoints,
    cv::Mat(),
    cv::PCA::DATA_AS_ROW
);
```

Lấy tâm:

``` cpp
cv::Point2f center(
    (float)pca.mean.at<double>(0, 0),
    (float)pca.mean.at<double>(0, 1)
);
```

Lấy trục chính:

``` cpp
cv::Point2f axis(
    (float)pca.eigenvectors.at<double>(0, 0),
    (float)pca.eigenvectors.at<double>(0, 1)
);
```

`axis` là hướng của trục dài nhất.

------------------------------------------------------------------------

# 8. Lưu ý rất quan trọng về PCA

PCA chỉ cho biết:

``` text
<-------------------->
        axis
```

Nó **không biết đầu nào là TOP và đầu nào là BOTTOM**.

Hai hướng:

``` text
TOP ---> BOTTOM
```

và

``` text
BOTTOM ---> TOP
```

đều là cùng một trục PCA.

Vì vậy cần thêm bước **FindBottomDirection()**.

------------------------------------------------------------------------

# 9. Bước 4 - Xác định đầu BOTTOM

Đây là phần cần calibration theo sản phẩm thực tế.

Một cách dễ thử trước là chia contour theo projection lên trục PCA.

Với mỗi điểm:

``` cpp
cv::Point2f d =
    cv::Point2f(
        point.x - center.x,
        point.y - center.y
    );

double projection =
    d.x * axis.x +
    d.y * axis.y;
```

Điểm có projection lớn nằm ở một đầu.

Điểm có projection nhỏ nằm ở đầu còn lại.

Ta chia thành:

``` text
         END A
           |
           |
      center
           |
           |
         END B
```

Sau đó lấy một vùng ở END A và END B để tính đặc trưng.

Ví dụ đặc trưng đơn giản:

-   độ rộng đầu,
-   diện tích vùng đầu,
-   độ cong,
-   khoảng cách biên trái/phải,
-   profile width.

Nếu đã biết mẫu OK và biết đầu nào là BOTTOM, có thể so sánh profile với
mẫu OK.

------------------------------------------------------------------------

# 10. Cách thực tế hơn: dùng Width Profile

Đây là phương pháp rất phù hợp với lỗi của sản phẩm bạn.

Sau khi biết trục:

``` text
TOP
 |
 |
 |
 |
BOTTOM
```

ta lấy nhiều lát cắt vuông góc với trục.

Ví dụ:

``` text
         |<------ width ------>|
         +---------------------+
         |                     |
         +---------------------+
         |                     |
         +---------------------+
         |                     |
         +---------------------+
```

Mỗi vị trí dọc theo sản phẩm có:

``` cpp
width[i]
```

Với sản phẩm OK:

``` text
width

████████████████████
████████████████████
████████████████████
████████████████████
████████████████████
```

Với NG bo góc:

``` text
████████████████████
████████████████████
 ██████████████████
  ████████████████
   ██████████████
```

Do đó có thể phát hiện NG bằng độ giảm width ở vùng BOTTOM.

------------------------------------------------------------------------

# 11. Không nên dùng toàn bộ BOTTOM

Chỉ kiểm tra một vùng nhỏ gần BOTTOM.

Ví dụ sản phẩm được normalize về chiều dài:

``` text
0%                  100%
TOP ---------------- BOTTOM

                     |<----|
                     vùng kiểm tra
```

Ví dụ:

``` cpp
double bottomStart = 0.80;
```

tức kiểm tra 20% cuối sản phẩm.

Có thể bắt đầu thử:

``` text
BOTTOM_ZONE = 15% ~ 25%
```

Sau đó điều chỉnh theo dữ liệu thực tế.

------------------------------------------------------------------------

# 12. Tách Bottom-Left và Bottom-Right

Trong Product Coordinate:

``` text
                 TOP
                  ^
                  |
        TL +-------------+ TR
           |             |
           |             |
           |             |
        BL +-------------+ BR
                  |
                  v
                BOTTOM
```

Ta không dùng:

``` cpp
point.y
```

của ảnh camera.

Thay vào đó dùng:

``` cpp
localX
localY
```

------------------------------------------------------------------------

# 13. Transform điểm sang Product Coordinate

Giả sử:

``` cpp
cv::Point2f right;
cv::Point2f bottom;
```

Trong đó:

-   `right` là vector hướng TOP -\> RIGHT.
-   `bottom` là vector hướng TOP -\> BOTTOM.

Với một điểm contour:

``` cpp
cv::Point2f d = point - center;

double localX =
    d.x * right.x +
    d.y * right.y;

double localY =
    d.x * bottom.x +
    d.y * bottom.y;
```

Kết quả:

``` text
localX < 0    LEFT
localX > 0    RIGHT

localY < 0    TOP
localY > 0    BOTTOM
```

------------------------------------------------------------------------

# 14. Tạo vector RIGHT từ vector BOTTOM

Nếu:

``` cpp
cv::Point2f bottom;
```

là vector đơn vị:

``` text
        TOP
         |
         |
         v
       BOTTOM
```

thì vector vuông góc:

``` cpp
cv::Point2f right(
    -bottom.y,
     bottom.x
);
```

hoặc đảo dấu nếu hướng LEFT/RIGHT bị ngược.

Sau đó phải kiểm tra hướng bằng một đặc trưng bất đối xứng của sản phẩm.

------------------------------------------------------------------------

# 15. Kiểm tra đúng Bottom-Left / Bottom-Right

Sau khi transform toàn bộ contour sang local coordinate:

``` cpp
struct LocalPoint
{
    double x;
    double y;
};
```

Ta có thể chia:

``` text
BOTTOM ZONE:

              center X
                 |
      LEFT       |       RIGHT
                 |
       BL        |        BR
```

Ví dụ:

``` cpp
if (localY > bottomThreshold)
{
    if (localX < 0)
    {
        // Bottom-Left
    }
    else
    {
        // Bottom-Right
    }
}
```

Như vậy dù sản phẩm nghiêng:

``` text
Camera:

       /
      /
     /
    /
```

vẫn nhận diện đúng:

``` text
Product:

       TOP
        ^
        |
   BL --+-- BR
        |
        v
      BOTTOM
```

------------------------------------------------------------------------

# 16. Phát hiện bo góc bằng khoảng cách tới biên OK

Đây là phương án mình khuyến nghị để làm bản đầu tiên.

Không cần tìm "góc hình học" hoàn hảo.

Ta lấy một mẫu OK chuẩn.

Sau khi normalize:

``` text
OK profile

xLeftOK[y]
xRightOK[y]
```

Với sản phẩm test:

``` text
xLeftTest[y]
xRightTest[y]
```

Tính:

``` cpp
double diffLeft =
    std::abs(xLeftTest - xLeftOK);

double diffRight =
    std::abs(xRightTest - xRightOK);
```

Nếu NG bị bo:

``` text
OK                     NG

|                      |
|                      |
|                      \
|                       \
|                        \
```

thì:

``` text
diff > threshold
```

ở vùng BOTTOM.

------------------------------------------------------------------------

# 17. Vì sản phẩm có thể thay đổi vị trí, phải normalize

Không được so sánh:

``` cpp
xTest == xOK
```

vì sản phẩm có thể lệch camera.

Thay vào đó:

1.  tìm bounding box;
2.  tìm center;
3.  xác định orientation;
4.  chuyển sang Product Coordinate;
5.  normalize chiều dài/rộng nếu cần.

Ví dụ chiều dài chuẩn hóa:

``` cpp
double t =
    (localY - minY) /
    (maxY - minY);
```

Khi đó:

``` text
t = 0.0  -> TOP
t = 1.0  -> BOTTOM
```

Vùng kiểm tra:

``` cpp
t > 0.80
```

sẽ luôn là BOTTOM dù sản phẩm nghiêng.

------------------------------------------------------------------------

# 18. Cách đơn giản hơn để bắt đầu

Đừng làm ngay thuật toán phức tạp.

Làm theo 4 bước thử nghiệm.

## Test 1 - Vẽ orientation

Sau khi PCA, vẽ:

``` cpp
cv::line(
    frame,
    center,
    center + axis * 200.0f,
    cv::Scalar(0, 0, 255),
    2
);
```

Mục tiêu:

> Kiểm tra xem trục có bám đúng theo chiều dài sản phẩm không.

------------------------------------------------------------------------

## Test 2 - Vẽ Product Coordinate

Vẽ:

``` text
TOP
 ^
 |
 +------> RIGHT
 |
 v
BOTTOM
```

lên ảnh.

Mục tiêu:

> Khi sản phẩm nghiêng lớn nhất, hệ trục vẫn phải đi theo sản phẩm.

------------------------------------------------------------------------

## Test 3 - Vẽ Bottom Zone

Ví dụ:

``` cpp
if (localY > 0.80 * length)
{
    cv::circle(...);
}
```

Mục tiêu:

> Tất cả điểm được đánh dấu phải nằm ở đúng đầu BOTTOM.

------------------------------------------------------------------------

## Test 4 - Vẽ Bottom-Left / Bottom-Right

``` cpp
if (localY > bottomZone)
{
    if (localX < 0)
        // màu A

    else
        // màu B
}
```

Mục tiêu:

> Không được có điểm ở TOP bị nhận nhầm thành Bottom-Left/Bottom-Right.

Chỉ khi 4 test này đúng mới thêm logic NG.

------------------------------------------------------------------------

# 19. Pseudocode hoàn chỉnh

``` text
INPUT IMAGE
     |
     v
Crop ROI
     |
     v
Threshold
     |
     v
Morphology
     |
     v
Find largest contour
     |
     v
Calculate PCA
     |
     v
Get main axis
     |
     v
Determine Bottom direction
     |
     v
Create Right/Bottom vectors
     |
     v
Transform contour to local coordinates
     |
     v
Normalize local Y
     |
     +--------------------+
     |                    |
     v                    v
Bottom Left          Bottom Right
     |                    |
     v                    v
Compare with OK      Compare with OK
     |                    |
     +---------+----------+
               |
               v
        threshold exceeded?
             /     \
           YES      NO
            |        |
            v        v
           NG        OK
```

------------------------------------------------------------------------

# 20. Khung code C++ ban đầu

Có thể bắt đầu bằng cấu trúc này:

``` cpp
#include <opencv2/opencv.hpp>
#include <iostream>
#include <vector>
#include <cmath>

struct ProductPose
{
    cv::Point2f center;

    // Product coordinate
    // right: local +X
    // bottom: local +Y
    cv::Point2f right;
    cv::Point2f bottom;
};

cv::Point2f toLocal(
    const cv::Point2f& p,
    const ProductPose& pose)
{
    cv::Point2f d = p - pose.center;

    return cv::Point2f(
        d.x * pose.right.x +
        d.y * pose.right.y,

        d.x * pose.bottom.x +
        d.y * pose.bottom.y
    );
}

int main()
{
    cv::Mat image =
        cv::imread("test.jpg");

    if (image.empty())
    {
        std::cerr << "Cannot open image\n";
        return -1;
    }

    // -------------------------------------------------
    // 1. Pre-processing
    // -------------------------------------------------

    cv::Mat gray;
    cv::cvtColor(
        image,
        gray,
        cv::COLOR_BGR2GRAY
    );

    cv::Mat binary;

    cv::threshold(
        gray,
        binary,
        120,
        255,
        cv::THRESH_BINARY
    );

    // -------------------------------------------------
    // 2. Morphology
    // -------------------------------------------------

    cv::Mat kernel =
        cv::getStructuringElement(
            cv::MORPH_ELLIPSE,
            cv::Size(5, 5)
        );

    cv::morphologyEx(
        binary,
        binary,
        cv::MORPH_CLOSE,
        kernel
    );

    // -------------------------------------------------
    // 3. Find contour
    // -------------------------------------------------

    std::vector<std::vector<cv::Point>> contours;

    cv::findContours(
        binary,
        contours,
        cv::RETR_EXTERNAL,
        cv::CHAIN_APPROX_NONE
    );

    if (contours.empty())
    {
        std::cerr << "No contour\n";
        return -1;
    }

    int bestIndex = -1;
    double bestArea = 0;

    for (int i = 0; i < (int)contours.size(); ++i)
    {
        double area =
            cv::contourArea(contours[i]);

        if (area > bestArea)
        {
            bestArea = area;
            bestIndex = i;
        }
    }

    std::vector<cv::Point> contour =
        contours[bestIndex];

    // -------------------------------------------------
    // 4. PCA
    // -------------------------------------------------

    cv::Mat data(
        (int)contour.size(),
        2,
        CV_64F
    );

    for (int i = 0; i < (int)contour.size(); ++i)
    {
        data.at<double>(i, 0) =
            contour[i].x;

        data.at<double>(i, 1) =
            contour[i].y;
    }

    cv::PCA pca(
        data,
        cv::Mat(),
        cv::PCA::DATA_AS_ROW
    );

    ProductPose pose;

    pose.center = cv::Point2f(
        (float)pca.mean.at<double>(0, 0),
        (float)pca.mean.at<double>(0, 1)
    );

    cv::Point2f axis(
        (float)pca.eigenvectors.at<double>(0, 0),
        (float)pca.eigenvectors.at<double>(0, 1)
    );

    // -------------------------------------------------
    // 5. IMPORTANT:
    //    Determine which direction is BOTTOM.
    //
    //    For first test, this may need to be manually
    //    reversed depending on the product.
    // -------------------------------------------------

    pose.bottom = axis;

    // Make +Y = BOTTOM.
    //
    // This is only a temporary direction.
    // Production code should determine this from
    // product asymmetry / reference profile.

    // -------------------------------------------------
    // 6. Create +X = RIGHT
    // -------------------------------------------------

    pose.right =
        cv::Point2f(
            -pose.bottom.y,
             pose.bottom.x
        );

    // -------------------------------------------------
    // 7. Transform contour
    // -------------------------------------------------

    std::vector<cv::Point2f> localContour;

    localContour.reserve(contour.size());

    for (const auto& p : contour)
    {
        localContour.push_back(
            toLocal(
                cv::Point2f(
                    (float)p.x,
                    (float)p.y
                ),
                pose
            )
        );
    }

    // -------------------------------------------------
    // 8. Find local bounds
    // -------------------------------------------------

    double minX = 1e20;
    double maxX = -1e20;

    double minY = 1e20;
    double maxY = -1e20;

    for (const auto& p : localContour)
    {
        minX = std::min(minX, (double)p.x);
        maxX = std::max(maxX, (double)p.x);

        minY = std::min(minY, (double)p.y);
        maxY = std::max(maxY, (double)p.y);
    }

    double length =
        maxY - minY;

    if (length <= 0)
    {
        return -1;
    }

    // -------------------------------------------------
    // 9. Find BOTTOM zone
    // -------------------------------------------------

    double bottomStart =
        minY + length * 0.80;

    for (const auto& p : localContour)
    {
        if (p.y >= bottomStart)
        {
            // Bottom-Left
            if (p.x < 0)
            {
                // check BL
            }

            // Bottom-Right
            else
            {
                // check BR
            }
        }
    }

    // -------------------------------------------------
    // Debug
    // -------------------------------------------------

    cv::circle(
        image,
        pose.center,
        5,
        cv::Scalar(0, 255, 0),
        -1
    );

    cv::line(
        image,
        pose.center,
        pose.center +
            pose.bottom * 200.0f,
        cv::Scalar(255, 0, 0),
        2
    );

    cv::line(
        image,
        pose.center,
        pose.center +
            pose.right * 200.0f,
        cv::Scalar(0, 0, 255),
        2
    );

    cv::imshow("Debug", image);
    cv::waitKey(0);

    return 0;
}
```

**Lưu ý:** phần `pose.bottom = axis;` ở trên chỉ là bước thử nghiệm. Nó
chưa giải quyết hoàn chỉnh vấn đề TOP/BOTTOM vì PCA không biết chiều.
Khi chạy được orientation, bước tiếp theo phải thay bằng
`FindBottomDirection()`.

------------------------------------------------------------------------

# 21. Tôi khuyên không nên dùng `approxPolyDP()` làm thuật toán chính

Có thể bạn đang nghĩ đến:

``` cpp
cv::approxPolyDP()
```

để tìm 4 góc.

Không nên phụ thuộc hoàn toàn vào nó trong case này.

Lý do:

-   góc sản phẩm có thể bị bo;
-   contour thực tế có noise;
-   sản phẩm nghiêng;
-   threshold có thể làm biên thay đổi;
-   `epsilon` thay đổi sẽ làm số đỉnh thay đổi.

`approxPolyDP()` có thể dùng để debug, nhưng **profile của contour** sẽ
phù hợp hơn cho việc phát hiện bo góc.

------------------------------------------------------------------------

# 22. Cách mình sẽ triển khai thực tế

Không nên code một lần tất cả.

## Phase 1

Chỉ làm:

``` text
Contour
→ PCA
→ Product axis
→ Debug image
```

Kiểm tra 20-50 ảnh với các góc nghiêng khác nhau.

------------------------------------------------------------------------

## Phase 2

Làm:

``` text
Product axis
→ Bottom direction
→ Product coordinate
→ Bottom zone
```

Vẽ debug lên ảnh.

Nếu nhìn bằng mắt thấy:

``` text
BOTTOM
```

luôn đúng thì mới tiếp tục.

------------------------------------------------------------------------

## Phase 3

Làm:

``` text
Bottom
→ Left / Right
→ Corner ROI
```

Vẫn chưa cần OK/NG.

------------------------------------------------------------------------

## Phase 4

Lấy khoảng:

-   20 ảnh OK,
-   20 ảnh NG,

và tính profile.

Không nên lấy threshold từ một ảnh duy nhất.

Ví dụ:

``` text
OK #1
OK #2
OK #3
...
OK #20

        ↓

OK reference profile
```

------------------------------------------------------------------------

## Phase 5

Đặt threshold:

``` text
diff < threshold → OK
diff >= threshold → NG
```

Threshold nên được lấy từ dữ liệu thực tế.

------------------------------------------------------------------------

# 23. Một vấn đề rất quan trọng: ảnh calibration

Nếu camera và băng chuyền cố định, nên tạo một bộ ảnh calibration:

``` text
OK:
0°
10°
20°
30°
40°
...
góc nghiêng lớn nhất
```

Không cần đúng chính xác từng 10°.

Mục tiêu là kiểm tra:

> Thuật toán có nhận diện đúng Product Coordinate ở toàn bộ range
> orientation không?

Đặc biệt phải test **góc nghiêng lớn nhất** mà bạn vừa gửi.

------------------------------------------------------------------------

# 24. Khi nào OpenCV thuần không đủ?

Nếu sau khi làm profile mà:

-   ánh sáng thay đổi mạnh,
-   nền thay đổi,
-   sản phẩm bị che,
-   contour bị đứt,
-   sản phẩm biến dạng nhiều,
-   OK và NG khác nhau rất ít,

thì mới cân nhắc ML/YOLO.

Nhưng với lỗi hiện tại là:

> "NG bị bo 2 góc nhưng vision đang bắt OK"

thì mình sẽ **ưu tiên giải quyết bằng geometry + contour profile
trước**.

------------------------------------------------------------------------

# 25. Kết luận

Với bài toán của bạn, hướng nên đi là:

``` text
                 KHÔNG
       Camera X/Y → tìm góc dưới
                  ❌


                 NÊN

Camera image
     ↓
Contour
     ↓
Orientation
     ↓
BOTTOM direction
     ↓
Product Coordinate
     ↓
Bottom-Left / Bottom-Right
     ↓
Corner/Profile comparison
     ↓
OK / NG
```

Điểm quan trọng nhất:

> **Sản phẩm nghiêng bao nhiêu độ không quan trọng bằng việc ta có xác
> định đúng hệ tọa độ của sản phẩm hay không.**

Và vì bạn đã quen C++/OpenCV, mình khuyên bắt đầu từ phương án này
trước, chưa cần YOLO.

## Việc tiếp theo nên làm

Để biến tài liệu này thành code chạy được trên dây chuyền của bạn, hãy
chuẩn bị khoảng:

-   5-10 ảnh OK ở các góc nghiêng khác nhau.
-   5-10 ảnh NG ở các góc tương ứng.
-   Tốt nhất giữ nguyên ảnh gốc từ camera, không resize/chụp màn hình.
-   Nếu có thể, đánh dấu trong ảnh đâu là **2 góc bị bo của NG**.

Sau đó có thể xây dựng `ProductPose`, `FindBottomDirection()` và
`CheckBottomCorners()` thành 3 hàm riêng, rồi tích hợp trực tiếp vào
project OpenCV hiện tại.
